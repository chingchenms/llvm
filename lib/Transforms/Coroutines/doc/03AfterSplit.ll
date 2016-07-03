%fib.frame = type { void (%fib.frame*)*, void (%fib.frame*)*, i32, %"struct.minig::promise_type", i32, %"struct.std::suspend_always", %"struct.std::suspend_always" }

; Function Attrs: alwaysinline nounwind
define void @fib(%struct.minig* noalias sret %agg.result) #0 {
entry:
  %0 = bitcast void (%fib.frame*)* @fib.cleanup to i8*
  %1 = call i1 @llvm.coro.suspend(i8* %0, i8* null, i1 true)
  %2 = zext i32 32 to i64
  %call = call noalias i8* @_Znwm(i64 %2)
  %3 = call i8* @llvm.coro.init(i8* %call)
  %frame = bitcast i8* %3 to %fib.frame*
  %__return = alloca %struct.minig, align 8
  %ref.tmp = alloca %"struct.std::suspend_always", align 1
  %__promise8 = getelementptr %fib.frame, %fib.frame* %frame, i32 0, i32 3
  call void @_ZN5minig12promise_type17get_return_objectEv(%struct.minig* sret %__return, %"struct.minig::promise_type"* %__promise8)
  %4 = getelementptr %fib.frame, %fib.frame* %frame, i32 0, i32 0
  store void (%fib.frame*)* @fib.resume, void (%fib.frame*)** %4
  %5 = getelementptr %fib.frame, %fib.frame* %frame, i32 0, i32 1
  store void (%fib.frame*)* @fib.destroy, void (%fib.frame*)** %5
  br label %init.suspend

init.suspend:                                     ; preds = %entry
  %6 = bitcast %"struct.std::suspend_always"* %ref.tmp to i8*
  %7 = getelementptr %fib.frame, %fib.frame* %frame, i32 0, i32 2
  store i32 0, i32* %7
  call void @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_(i8* %6, i8* %3)
  br label %coro.ret

coro.ret:                                         ; preds = %init.suspend
  call void @_ZN5minigC1EOS_(%struct.minig* %agg.result, %struct.minig* dereferenceable(8) %__return)
  call void @_ZN5minigD1Ev(%struct.minig* %__return) #3
  ret void
}

define internal fastcc void @fib.resume(%fib.frame* %frame.ptr) {
entry:
  %frame.void.ptr = bitcast %fib.frame* %frame.ptr to i8*
  %0 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 2
  %resume.index = load i32, i32* %0
  switch i32 %resume.index, label %unreachable [
    i32 0, label %init.ready
    i32 2, label %for.inc
  ]

init.ready:                                       ; preds = %entry
  %i11 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 4
  store i32 0, i32* %i11, align 4, !tbaa !9
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %init.ready
  %i10 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 4
  %1 = load i32, i32* %i10, align 4, !tbaa !9
  %cmp = icmp slt i32 %1, 5
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  %ref.tmp5 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 6
  %2 = bitcast %"struct.std::suspend_always"* %ref.tmp5 to i8*
  %3 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 0
  store void (%fib.frame*)* null, void (%fib.frame*)** %3
  call void @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_(i8* %2, i8* %frame.void.ptr)
  ret void

for.body:                                         ; preds = %for.cond
  %__promise = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 3
  call void @_ZN5minig12promise_type11yield_valueEi(%"struct.minig::promise_type"* %__promise, i32 %1)
  %ref.tmp2 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 5
  %4 = bitcast %"struct.std::suspend_always"* %ref.tmp2 to i8*
  %5 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 2
  store i32 2, i32* %5
  call void @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_(i8* %4, i8* %frame.void.ptr)
  ret void

for.inc:                                          ; preds = %entry
  %i9 = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 4
  %6 = load i32, i32* %i9, align 4, !tbaa !9
  %inc = add nsw i32 %6, 1
  %i = getelementptr %fib.frame, %fib.frame* %frame.ptr, i32 0, i32 4
  store i32 %inc, i32* %i, align 4, !tbaa !9
  br label %for.cond

unreachable:                                      ; preds = %entry
  unreachable
}

define internal fastcc void @fib.cleanup(%fib.frame* %frame.ptr) {
entry:
  %frame.void.ptr = bitcast %fib.frame* %frame.ptr to i8*
  br label %ret

ret:                                              ; preds = %entry
  ret void
}

define internal fastcc void @fib.destroy(%fib.frame* %frame.ptr) {
entry:
  %frame.void.ptr = bitcast %fib.frame* %frame.ptr to i8*
  call fastcc void @fib.cleanup(%fib.frame* %frame.ptr)
  br label %coro.destroy.label

coro.destroy.label:                               ; preds = %entry
  call void @_ZdlPv(i8* %frame.void.ptr) #3
  ret void
}
