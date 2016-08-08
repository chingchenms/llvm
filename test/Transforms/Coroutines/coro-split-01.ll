; RUN: opt < %s -S -enable-coroutines -O2 | FileCheck %s

define i8* @f() {
entry:  
  %elide = call i8* @llvm.coro.alloc()
  %need.dyn.alloc = icmp ne i8* %elide, null
  br i1 %need.dyn.alloc, label %coro.begin, label %dyn.alloc
dyn.alloc:
  %size = call i32 @llvm.coro.size.i32()
  %alloc = call i8* @malloc(i32 %size)
  br label %coro.begin
coro.begin:
  %phi = phi i8* [ %elide, %entry ], [ %alloc, %dyn.alloc ]
  %hdl = call i8* @llvm.coro.begin(i8* %phi, i32 0, i8* null, i8* null)
  call void @print(i32 0)
  %0 = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %0, label %suspend [i8 0, label %resume 
                                i8 1, label %cleanup]
resume:
  call void @print(i32 1)
  br label %cleanup

cleanup:
  %mem = call i8* @llvm.coro.free(i8* %hdl)
  call void @free(i8* %mem)
  br label %suspend
suspend:
  call void @llvm.coro.end(i8* %hdl, i1 0)  
  ret i8* %hdl
}
define i32 @main() {
entry:
  %hdl = call i8* @f()
  call void @llvm.coro.resume(i8* %hdl)
  ret i32 0
; CHECK-LABEL: @main(
; CHECK:      call i8* @malloc
; CHECK-NOT:  call @free
; CHECK: call void @print(i32 0)
; CHECK-NOT:  call @free
; CHECK: call void @print(i32 1)
; CHECK:      call @free
; CHECK:      ret i32 0
}

declare i8* @llvm.coro.alloc()
declare i8* @llvm.coro.free(i8*)
declare i32 @llvm.coro.size.i32()
declare i8  @llvm.coro.suspend(token, i1)
declare void @llvm.coro.resume(i8*)
declare void @llvm.coro.destroy(i8*)
  
declare i8* @llvm.coro.begin(i8*, i32, i8*, i8*)
declare void @llvm.coro.end(i8*, i1) 

declare noalias i8* @malloc(i32)
declare void @print(i32)
declare void @free(i8*)
