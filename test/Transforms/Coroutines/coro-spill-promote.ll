; Check that we do not spill cleanup-slot like allocas.
; RUN: opt < %s -coro-split -S | FileCheck %s

define i8* @happy.case() "coroutine.presplit"="1" {
entry:
  %slot = alloca i32
  %id = call token @llvm.coro.id(i32 0, i8* null, i8* null, i8* null)
  %size = call i32 @llvm.coro.size.i32()
  %alloc = call i8* @malloc(i32 %size)
  %hdl = call i8* @llvm.coro.begin(token %id, i8* %alloc)

  store i32 1, i32* %slot

  %tok = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %tok, label %suspend [i8 0, label %resume
                                i8 1, label %cleanup]
resume:
  store i32 2, i32* %slot
  br label %cleanup

cleanup:
  %mem = call i8* @llvm.coro.free(token %id, i8* %hdl)
  call void @free(i8* %mem)
  %x = load i32, i32* %slot
  call void @print.i32(i32 %x)
  br label %suspend
suspend:
  call i1 @llvm.coro.end(i8* %hdl, i1 0)
  ret i8* %hdl
}

; Verify that we did not spill i32 slot alloca into the frame
; CHECK: %happy.case.Frame = type { void (%happy.case.Frame*)*, void (%happy.case.Frame*)*, i1, i1 }

; Verify that we spilled alloca due to escape.
; CHECK: %escaped.case.Frame = type { void (%escaped.case.Frame*)*, void (%escaped.case.Frame*)*, i1, i1, i32 }

; Verify that the promise was spilled to the frame (3rd field in the frame struct)
; %promise.case.Frame = type { void (%promise.case.Frame*)*, void (%promise.case.Frame*)*, i32, i1 }

; See that in a resume function we are getting constant 2
; CHECK-LABEL: define internal fastcc void @happy.case.resume(
; CHECK:         call void @free(i8* {{.+}})
; CHECK-NEXT:    call void @print.i32(i32 2)
; CHECK-NEXT:    ret void

; See that in a destroy function we are getting constant 1
; CHECK-LABEL: define internal fastcc void @happy.case.destroy(
; CHECK:         call void @free(i8* {{.+}})
; CHECK-NEXT:    call void @print.i32(i32 1)
; CHECK-NEXT:    ret void

define i8* @escaped.case() "coroutine.presplit"="1" {
entry:
  %slot = alloca i32
  %id = call token @llvm.coro.id(i32 0, i8* null, i8* null, i8* null)
  %size = call i32 @llvm.coro.size.i32()
  %alloc = call i8* @malloc(i32 %size)
  %hdl = call i8* @llvm.coro.begin(token %id, i8* %alloc)

  store i32 1, i32* %slot
  call void @escape(i32* %slot)

  %tok = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %tok, label %suspend [i8 0, label %resume
                                i8 1, label %cleanup]
resume:
  store i32 2, i32* %slot
  br label %cleanup

cleanup:
  %mem = call i8* @llvm.coro.free(token %id, i8* %hdl)
  %x = load i32, i32* %slot
  call void @print.i32(i32 %x)
  call void @free(i8* %mem)
  br label %suspend
suspend:
  call i1 @llvm.coro.end(i8* %hdl, i1 0)
  ret i8* %hdl
}

define i8* @promise.case() "coroutine.presplit"="1" {
entry:
  %slot = alloca i32
  %pv = bitcast i32* %slot to i8*
  %id = call token @llvm.coro.id(i32 0, i8* %pv, i8* null, i8* null)
  %size = call i32 @llvm.coro.size.i32()
  %alloc = call i8* @malloc(i32 %size)
  %hdl = call i8* @llvm.coro.begin(token %id, i8* %alloc)

  %tok = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %tok, label %suspend [i8 0, label %resume
                                i8 1, label %cleanup]
resume:
  store i32 2, i32* %slot
  br label %cleanup

cleanup:
  %mem = call i8* @llvm.coro.free(token %id, i8* %hdl)
  call void @free(i8* %mem)
  br label %suspend
suspend:
  call i1 @llvm.coro.end(i8* %hdl, i1 0)
  ret i8* %hdl
}

declare i8* @llvm.coro.free(token, i8*)
declare i32 @llvm.coro.size.i32()
declare i8  @llvm.coro.suspend(token, i1)

declare token @llvm.coro.id(i32, i8*, i8*, i8*)
declare i1 @llvm.coro.alloc(token)
declare i8* @llvm.coro.begin(token, i8*)
declare i1 @llvm.coro.end(i8*, i1)

declare noalias i8* @malloc(i32)
declare void @print.i32(i32)
declare void @escape(i32*)
declare void @free(i8*)
