; Function Attrs: nounwind
define void @fib(%struct.minig* noalias sret %agg.result) #0 {
entry:
  %__promise = alloca %"struct.minig::promise_type", align 4
  %__return = alloca %struct.minig, align 8
  %ref.tmp = alloca %"struct.std::suspend_always", align 1
  %undef.agg.tmp = alloca %"struct.std::suspend_always", align 1
  %i = alloca i32, align 4
  %ref.tmp2 = alloca %"struct.std::suspend_always", align 1
  %undef.agg.tmp3 = alloca %"struct.std::suspend_always", align 1
  %ref.tmp5 = alloca %"struct.std::suspend_always", align 1
  %undef.agg.tmp6 = alloca %"struct.std::suspend_always", align 1
  %0 = call i32 @llvm.coro.size()
  %1 = zext i32 %0 to i64
  %call = call noalias i8* @_Znwm(i64 %1)
  %2 = call i8* @llvm.coro.init(i8* %call)
  %3 = bitcast %"struct.minig::promise_type"* %__promise to i8*
  call void @llvm.lifetime.start(i64 4, i8* %3) #3
  %4 = bitcast %struct.minig* %__return to i8*
  call void @llvm.lifetime.start(i64 8, i8* %4) #3
  call void @_ZN5minig12promise_type17get_return_objectEv(%struct.minig* sret %__return, %"struct.minig::promise_type"* %__promise)
  %5 = call i1 @llvm.coro.done(i8* null)
  br i1 %5, label %coro.ret, label %coro.start

coro.start:                                       ; preds = %entry
  call void @_ZN5minig12promise_type15initial_suspendEv(%"struct.minig::promise_type"* %__promise)
  %call1 = call zeroext i1 @_ZNSt14suspend_always11await_readyEv(%"struct.std::suspend_always"* %ref.tmp)
  br i1 %call1, label %init.ready, label %init.suspend

init.suspend:                                     ; preds = %coro.start
  %6 = bitcast %"struct.std::suspend_always"* %ref.tmp to i8*
  %7 = call i1 @llvm.coro.suspend(i8* %6, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %7, label %init.ready, label %init.cleanup

init.cleanup:                                     ; preds = %init.suspend
  br label %coro.destroy.label

init.ready:                                       ; preds = %init.suspend, %coro.start
  call void @_ZNSt14suspend_always12await_resumeEv(%"struct.std::suspend_always"* %ref.tmp)
  %8 = bitcast i32* %i to i8*
  call void @llvm.lifetime.start(i64 4, i8* %8) #3
  store i32 0, i32* %i, align 4, !tbaa !1
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %init.ready
  %9 = load i32, i32* %i, align 4, !tbaa !1
  %cmp = icmp slt i32 %9, 5
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  %10 = bitcast i32* %i to i8*
  call void @llvm.lifetime.end(i64 4, i8* %10) #3
  br label %for.end

for.body:                                         ; preds = %for.cond
  %11 = load i32, i32* %i, align 4, !tbaa !1
  call void @_ZN5minig12promise_type11yield_valueEi(%"struct.minig::promise_type"* %__promise, i32 %11)
  %call4 = call zeroext i1 @_ZNSt14suspend_always11await_readyEv(%"struct.std::suspend_always"* %ref.tmp2)
  br i1 %call4, label %yield.ready, label %yield.suspend

yield.suspend:                                    ; preds = %for.body
  %12 = bitcast %"struct.std::suspend_always"* %ref.tmp2 to i8*
  %13 = call i1 @llvm.coro.suspend(i8* %12, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %13, label %yield.ready, label %yield.cleanup

yield.cleanup:                                    ; preds = %yield.suspend
  br label %coro.destroy.label

yield.ready:                                      ; preds = %yield.suspend, %for.body
  call void @_ZNSt14suspend_always12await_resumeEv(%"struct.std::suspend_always"* %ref.tmp2)
  br label %for.inc

for.inc:                                          ; preds = %yield.ready
  %14 = load i32, i32* %i, align 4, !tbaa !1
  %inc = add nsw i32 %14, 1
  store i32 %inc, i32* %i, align 4, !tbaa !1
  br label %for.cond

for.end:                                          ; preds = %for.cond.cleanup
  br label %coro.fin

coro.fin:                                         ; preds = %for.end
  call void @_ZN5minig12promise_type13final_suspendEv(%"struct.minig::promise_type"* %__promise)
  %call7 = call zeroext i1 @_ZNSt14suspend_always11await_readyEv(%"struct.std::suspend_always"* %ref.tmp5)
  br i1 %call7, label %final.ready, label %final.suspend

final.suspend:                                    ; preds = %coro.fin
  %15 = bitcast %"struct.std::suspend_always"* %ref.tmp5 to i8*
  %16 = call i1 @llvm.coro.suspend(i8* %15, i8* bitcast (void (i8*, i8*)* @_Z5_RampISt14suspend_alwaysN5minig12promise_typeEEvPvS3_ to i8*), i1 true)
  br i1 %16, label %coro.ret, label %final.cleanup

final.cleanup:                                    ; preds = %final.suspend
  br label %coro.destroy.label

final.ready:                                      ; preds = %coro.fin
  call void @_ZNSt14suspend_always12await_resumeEv(%"struct.std::suspend_always"* %ref.tmp5)
  br label %coro.destroy.label

coro.destroy.label:                               ; preds = %final.ready, %final.cleanup, %yield.cleanup, %init.cleanup
  %17 = call i8* @llvm.coro.frame()
  call void @_ZdlPv(i8* %17) #3
  br label %coro.ret

coro.ret:                                         ; preds = %coro.destroy.label, %final.suspend, %entry
  call void @_ZN5minigC1EOS_(%struct.minig* %agg.result, %struct.minig* dereferenceable(8) %__return)
  call void @_ZN5minigD1Ev(%struct.minig* %__return) #3
  %18 = bitcast %struct.minig* %__return to i8*
  call void @llvm.lifetime.end(i64 8, i8* %18) #3
  %19 = bitcast %"struct.minig::promise_type"* %__promise to i8*
  call void @llvm.lifetime.end(i64 4, i8* %19) #3
  ret void
}
