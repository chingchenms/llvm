; Fourth example from Doc/Coroutines.rst (coroutine promise)
; RUN: opt < %s -O3 -S | FileCheck %s

define i8* @f(i32 %n) {
entry:
  %promise = alloca i32
  %pv = bitcast i32* %promise to i8*
  %size = call i32 @llvm.coro.size.i32(i8* null)
  %alloc = call i8* @malloc(i32 %size)
  %hdl = call noalias i8* @llvm.coro.begin(i8* %alloc, i32 0, i8* %pv, i8* null)
  br label %loop
loop:
  %n.val = phi i32 [ %n, %entry ], [ %inc, %loop ]
  %inc = add nsw i32 %n.val, 1
  store i32 %n.val, i32* %promise ; storing the result in the promise
  %0 = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %0, label %suspend [i8 0, label %loop
                                i8 1, label %cleanup]
cleanup:
  %mem = call i8* @llvm.coro.free(i8* %hdl)
  call void @free(i8* %mem)
  br label %suspend
suspend:
  call void @llvm.coro.end(i8* %hdl, i1 false)
  ret i8* %hdl
}

; CHECK-LABEL: @main
define i32 @main() {
entry:
  %hdl = call i8* @f(i32 4)
  %promise.addr = call i32* @llvm.coro.promise.p0i32(i8* %hdl)
  %val0 = load i32, i32* %promise.addr
  call void @print(i32 %val0)
  call void @llvm.coro.resume(i8* %hdl)
  %val1 = load i32, i32* %promise.addr
  call void @print(i32 %val1)
  call void @llvm.coro.resume(i8* %hdl)
  %val2 = load i32, i32* %promise.addr
  call void @print(i32 %val2)
  call void @llvm.coro.destroy(i8* %hdl)
  ret i32 0
; CHECK: entry:
; CHECK-NEXT: call void @print(i32 4)
; CHECK-NEXT: call void @print(i32 5)
; CHECK-NEXT: call void @print(i32 6)
; CHECK-NEXT: ret i32 0
}

declare i32* @llvm.coro.promise.p0i32(i8*)
declare i8* @malloc(i32)
declare void @free(i8*)
declare void @print(i32)

declare i8* @llvm.coro.alloc()
declare i32 @llvm.coro.size.i32(i8*) #3
declare i8* @llvm.coro.begin(i8*, i32, i8*, i8*)
declare i8 @llvm.coro.suspend(token, i1)
declare i8* @llvm.coro.free(i8*)
declare void @llvm.coro.end(i8*, i1)

declare void @llvm.coro.resume(i8*)
declare void @llvm.coro.destroy(i8*)
