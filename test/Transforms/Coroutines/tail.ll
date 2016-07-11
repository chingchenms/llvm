; when we do heap elision, we need to remove tail-call property for any calls
; referencing the coroutine frame. We should also retain tail call for those
; that don't.
; RUN: opt < %s -O3 -S | FileCheck %s

@g = global i32 0, align 4

define i8* @f() coroutine {
entry:
  %i = alloca i32
  %0 = tail call i32 @llvm.coro.size.i32(i8* null)
  %call = tail call i8* @malloc(i32 %0)
  %1 = tail call i8* @llvm.coro.begin(i8* %call, i32 0, i8* null, i8* null)
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %storemerge = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
  store i32 %storemerge, i32* %i, align 4
  call void @print(i32* nonnull %i)
  %2 = call token @llvm.coro.save(i8* %1)
  %3 = call i8 @llvm.coro.suspend(token %2, i1 false)
  %conv = sext i8 %3 to i32
  switch i32 %conv, label %coro_Suspend [
    i32 0, label %for.inc
    i32 1, label %coro_Cleanup
  ]

for.inc:                                          ; preds = %for.cond
  %4 = load i32, i32* %i
  %inc = add nsw i32 %4, 1
  br label %for.cond

coro_Cleanup:                                     ; preds = %for.cond
  %5 = call i8* @llvm.coro.free(i8* %1)
  call void @free(i8* %5)
  br label %coro_Suspend

coro_Suspend:                                     ; preds = %for.cond, %coro_Cleanup
  call void @llvm.coro.end(i8* %1, i1 false)
  ret i8* %1
}

declare i8* @malloc(i32)
declare i32 @llvm.coro.size.i32(i8*)
declare i8* @llvm.coro.begin(i8*, i32, i8*, i8*)
declare void @print(i32*)
declare token @llvm.coro.save(i8*)
declare i8 @llvm.coro.suspend(token, i1)
declare void @free(i8*)
declare i8* @llvm.coro.free(i8*)
declare void @llvm.coro.end(i8*, i1)
declare void @llvm.coro.destroy(i8*)

; CHECK-LABEL: @main
define i32 @main() {
entry:
  %call = tail call i8* @f()
  tail call void @llvm.coro.destroy(i8* %call)
  tail call void @print(i32* nonnull @g)
  tail call void @print(i32* nonnull @g)
; CHECK:      store i32 0, i32* [[REG:%.+]], align 4
; CHECK-NOT:  tail
; CHECK-NEXT: call void @print(i32* nonnull [[REG]])
; CHECK-NEXT: tail call void @print(i32* nonnull @g)
; CHECK-NEXT: tail call void @print(i32* nonnull @g)
; CHECK-NEXT: ret i32 0
  ret i32 0
}
