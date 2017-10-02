; Check that we do not spill cleanup-slot like allocas.
; RUN: opt < %s -coro-early -S | FileCheck %s

define i8* @happy.case() {
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

; See that we got a PHI with constants in the happy case
; CHECK-LABEL: define i8* @happy.case(
; CHECK:         %[[SLOT:.+]] = phi i32 [ 1, %entry ], [ 2, %resume ]
; CHECK:         call void @free(i8* {{.+}})
; CHECK:         call void @print.i32(i32 %[[SLOT]])
; CHECK:         ret i8*

define i8* @escaped.case() {
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

; See that we kept the load when the alloca escaped
; CHECK-LABEL: define i8* @escaped.case(
; CHECK:         %[[SLOT:.+]] = load i32, i32* %{{.+}}
; CHECK:         call void @print.i32(i32 %[[SLOT]])
; CHECK:         ret i8*

define i8* @promise.case() {
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
  %x = load i32, i32* %slot
  call void @print.i32(i32 %x)
  %mem = call i8* @llvm.coro.free(token %id, i8* %hdl)
  call void @free(i8* %mem)
  br label %suspend
suspend:
  call i1 @llvm.coro.end(i8* %hdl, i1 0)
  ret i8* %hdl
}

; See that we kept the load when the alloca is from promise
; CHECK-LABEL: define i8* @promise.case(
; CHECK:         %[[SLOT:.+]] = load i32, i32* %{{.+}}
; CHECK:         call void @print.i32(i32 %[[SLOT]])
; CHECK:         ret i8*

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
