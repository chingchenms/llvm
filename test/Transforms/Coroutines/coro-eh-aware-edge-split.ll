; Check that we can handle edge splits leading into a landingpad
; RUN: opt < %s -coro-split -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%"struct.coro::promise_type" = type { i8 }

; Function Attrs: uwtable
define void @f(i1 %n) "coroutine.presplit"="1" personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
entry:
  %__promise = alloca %"struct.coro::promise_type", align 1
  %0 = getelementptr inbounds %"struct.coro::promise_type", %"struct.coro::promise_type"* %__promise, i64 0, i32 0
  %1 = call token @llvm.coro.id(i32 16, i8* nonnull %0, i8* null, i8* null)
  %2 = call i1 @llvm.coro.alloc(token %1)
  br i1 %2, label %coro.alloc, label %init.suspend

coro.alloc:                                       ; preds = %entry
  %3 = tail call i64 @llvm.coro.size.i64()
  %call = call i8* @_Znwm(i64 %3)
  br label %init.suspend

init.suspend:                                     ; preds = %entry, %coro.alloc
  %4 = phi i8* [ null, %entry ], [ %call, %coro.alloc ]
  %5 = call i8* @llvm.coro.begin(token %1, i8* %4)
  %6 = call token @llvm.coro.save(i8* null)
  %7 = call i8 @llvm.coro.suspend(token %6, i1 false)
  switch i8 %7, label %coro.ret [
    i8 0, label %init.ready
    i8 1, label %cleanup
  ]

init.ready:                                       ; preds = %init.suspend
  br i1 %n, label %if.else, label %if.then

if.then:                                          ; preds = %init.ready
  invoke void @_Z10may_throw1v()
          to label %unreach unwind label %lpad11

lpad11:                                           ; preds = %if.else, %if.then
  %val.0 = phi i32 [ 0, %if.then ], [ 1, %if.else ]
  %8 = landingpad { i8*, i32 }
          catch i8* null
  %9 = extractvalue { i8*, i32 } %8, 0
  %10 = call i8* @__cxa_begin_catch(i8* %9)
  call void @_Z3usei(i32 %val.0)
  call void @__cxa_end_catch()
  br label %cleanup

if.else:                                          ; preds = %init.ready
  invoke void @_Z10may_throw2v()
          to label %unreach unwind label %lpad11

cleanup:                                        ; preds = %invoke.cont15, %if.else, %if.then, %ehcleanup21, %init.suspend
  %mem = call i8* @llvm.coro.free(token %1, i8* %5)
  call void @_ZdlPv(i8* %mem)
  br label %coro.ret

coro.ret:
  call i1 @llvm.coro.end(i8* null, i1 false)
  ret void

unreach:
  unreachable
}

; See if we extracted resume part and dealt with phi in landing pad

; CHECK-LABEL: define internal fastcc void @f.resume(
; CHECK: lpad11.from.if.else:
; CHECK:   %0 = landingpad { i8*, i32 }
; CHECK:           catch i8* null
; CHECK:   br label %lpad11

; CHECK: lpad11.from.if.then:
; CHECK:   %1 = landingpad { i8*, i32 }
; CHECK:           catch i8* null
; CHECK:   br label %lpad11

; CHECK: lpad11:
; CHECK:   %val.0 = phi i32 [ 0, %lpad11.from.if.then ], [ 1, %lpad11.from.if.else ]
; CHECK:   %2 = phi { i8*, i32 } [ %0, %lpad11.from.if.else ], [ %1, %lpad11.from.if.then ]
; CHECK:   %3 = extractvalue { i8*, i32 } %2, 0
; CHECK:   %4 = call i8* @__cxa_begin_catch(i8* %3)
; CHECK:   call void @_Z3usei(i32 %val.0)
; CHECK:   call void @__cxa_end_catch()
; CHECK:   call void @_ZdlPv(i8* %vFrame)
; CHECK:   ret void

; CHECK: if.else:
; CHECK:   invoke void @_Z10may_throw2v()
; CHECK:           to label %unreach unwind label %lpad11.from.if.else

; Function Attrs: argmemonly nounwind readonly
declare token @llvm.coro.id(i32, i8* readnone, i8* nocapture readonly, i8*)
declare i1 @llvm.coro.alloc(token)
declare noalias i8* @_Znwm(i64)
declare i64 @llvm.coro.size.i64()
declare i8* @llvm.coro.begin(token, i8* writeonly)

declare i32 @__gxx_personality_v0(...)

; Function Attrs: nounwind
declare token @llvm.coro.save(i8*)
declare i8 @llvm.coro.suspend(token, i1)

; Function Attrs: argmemonly nounwind
declare void @_Z10may_throw1v()
declare void @_Z10may_throw2v()

declare i8* @__cxa_begin_catch(i8*)

declare void @_Z3usei(i32)
declare void @__cxa_end_catch()

; Function Attrs: nounwind
declare i1 @llvm.coro.end(i8*, i1)
declare void @_ZdlPv(i8*)
declare i8* @llvm.coro.free(token, i8* nocapture readonly)
declare i8* @llvm.coro.subfn.addr(i8* nocapture readonly, i8)
