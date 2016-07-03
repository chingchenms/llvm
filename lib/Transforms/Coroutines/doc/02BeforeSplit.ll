; Function Attrs: alwaysinline nounwind
define void @fib(%struct.minig* noalias sret %agg.result) #0 {
entry:
  %__promise = alloca %"struct.minig::promise_type", align 4
  %__return = alloca %struct.minig, align 8
  %ref.tmp = alloca %"struct.std::suspend_always", align 1
  %i = alloca i32, align 4
  %ref.tmp2 = alloca %"struct.std::suspend_always", align 1
  %ref.tmp5 = alloca %"struct.std::suspend_always", align 1
  %0 = call i32 @llvm.coro.size()
  %1 = zext i32 %0 to i64
  %call = call noalias i8* @_Znwm(i64 %1)
  %2 = call i8* @llvm.coro.init(i8* %call)
  call void @_ZN5minig12promise_type17get_return_objectEv(%struct.minig* sret %__return, %"struct.minig::promise_type"* %__promise)
  %3 = call i1 @llvm.coro.done(i8* null)
  br i1 %3, label %coro.ret, label %init.suspend

init.suspend:                                     ; preds = %entry
  %4 = bitcast %"struct.std::suspend_always"* %ref.tmp to i8*
  %5 = call i1 @llvm.coro.suspend(i8* %4, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %5, label %init.ready, label %coro.destroy.label

init.ready:                                       ; preds = %init.suspend
  store i32 0, i32* %i, align 4, !tbaa !1
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %init.ready
  %6 = load i32, i32* %i, align 4, !tbaa !1
  %cmp = icmp slt i32 %6, 5
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  %7 = bitcast %"struct.std::suspend_always"* %ref.tmp5 to i8*
  %8 = call i1 @llvm.coro.suspend(i8* %7, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %8, label %coro.ret, label %coro.destroy.label

for.body:                                         ; preds = %for.cond
  call void @_ZN5minig12promise_type11yield_valueEi(%"struct.minig::promise_type"* %__promise, i32 %6)
  %9 = bitcast %"struct.std::suspend_always"* %ref.tmp2 to i8*
  %10 = call i1 @llvm.coro.suspend(i8* %9, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %10, label %for.inc, label %coro.destroy.label

for.inc:                                          ; preds = %for.body
  %11 = load i32, i32* %i, align 4, !tbaa !1
  %inc = add nsw i32 %11, 1
  store i32 %inc, i32* %i, align 4, !tbaa !1
  br label %for.cond

coro.destroy.label:                               ; preds = %for.cond.cleanup, %for.body, %init.suspend
  %12 = call i8* @llvm.coro.frame()
  call void @_ZdlPv(i8* %12) #3
  br label %coro.ret

coro.ret:                                         ; preds = %coro.destroy.label, %for.cond.cleanup, %entry
  call void @_ZN5minigC1EOS_(%struct.minig* %agg.result, %struct.minig* dereferenceable(8) %__return)
  call void @_ZN5minigD1Ev(%struct.minig* %__return) #3
  ret void
}
