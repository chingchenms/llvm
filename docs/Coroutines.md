# LLVM Coroutines
## Introduction
LLVM coroutines are functions that have one or more suspend points. When a suspend point is reached, execution of a coroutine is suspended. A suspended coroutine can be resumed to continue execution from the last suspend point or be destroyed. 
bla bla
## Terms
* Coroutine Frame: storage that persist across coroutine suspends and resumes. 
* Coroutine Promise: library defined type that is part of the coroutine frame
* Coroutine Handle: a pointer that encodes information about an  
* Suspend Point
* Final Suspend point

## Coroutine structure intrinsics

Coroutine structure intrinsics are emitted by the front end to indicate where the suspend points are, which allocation and deallocation functions need to be used if storage for the coroutine frame needs to be allocated d

def int_experimental_coro_init : Intrinsic<[llvm_ptr_ty], [llvm_ptr_ty, llvm_ptr_ty, llvm_ptr_ty], []>;
def int_experimental_coro_fork : Intrinsic<[llvm_i1_ty], [], []>;

<!-- def int_experimental_coro_frame : Intrinsic<[llvm_ptr_ty], [], []>;-->
def int_experimental_coro_size : Intrinsic<[llvm_i32_ty], [], [IntrNoMem]>;

def int_experimental_coro_resume_end : Intrinsic<[], [], []>;

def int_experimental_coro_save : Intrinsic<[llvm_token_ty], [llvm_i32_ty], []>;
def int_experimental_coro_suspend : Intrinsic<[llvm_i1_ty], [llvm_token_ty], []>;

## Library support intrinsics
The following intrinsics are used by the libraries to create high-level coroutine abstractions, such as generator, tasks, etc.

### llvm.experimental.coro.resume
#### Syntax
    declare void
      @llvm.experimental.coro.resume(i8* %handle)
#### Overview
The `@llvm.experimental.coro.resume` intrinsics resumes a coroutine.
#### Arguments
The argument is a coroutine handle returned previously by `@llvm.experimental.coro.from.promise`.
#### Semantics
bla bla

def int_experimental_coro_resume : Intrinsic<[], [llvm_ptr_ty], [Throws]>;
def int_experimental_coro_destroy : Intrinsic<[], [llvm_ptr_ty], [Throws]>;
def int_experimental_coro_done : Intrinsic<[llvm_i1_ty], [llvm_ptr_ty], [IntrReadMem]>;
def int_experimental_coro_promise : Intrinsic<[llvm_anyptr_ty], [llvm_ptr_ty], []>;
def int_experimental_coro_from_promise : Intrinsic<[llvm_ptr_ty], [llvm_anyptr_ty], []>;

## Heap elision optimization intrinsics

def int_experimental_coro_elide : Intrinsic<[llvm_ptr_ty], [], []>;
def int_experimental_coro_delete : Intrinsic<[llvm_i1_ty], [llvm_ptr_ty], []>;

## Optimizations

## Coroutine Hello World

	declare noalias i8* @malloc(i32)
	declare void @free(i8*)
	declare void @yield(i32)
	
	declare void @llvm.experimental.coro.resume(i8*)
	declare void @llvm.experimental.coro.destroy(i8*)
	declare i8* @llvm.experimental.coro.init(i8*, i8*, i8*)
	declare i32 @llvm.experimental.coro.size()
	declare i1 @llvm.experimental.coro.fork()
	declare token @llvm.experimental.coro.save(i32)
	;declare i8* @llvm.experimental.coro.frame()
	declare i1 @llvm.experimental.coro.suspend(token)
	declare i8* @llvm.experimental.coro.delete(i8*)
	declare void @llvm.experimental.coro.resume.end()
	
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	
	define i8* @f(i32 %n) {
	entry:
	  %frame.size = call i32 @llvm.experimental.coro.size()
	  %alloc = call noalias i8* @malloc(i32 %frame.size)
	  %frame = call i8* @llvm.experimental.coro.init(i8* %alloc, i8* null, i8* null)
	  %first.return = call i1 @llvm.experimental.coro.fork()
	  br i1 %first.return, label %coro.return, label %coro.start
	
	coro.start:
	  %n.val = phi i32 [ %n, %entry ], [ %inc, %resume ]
	  call void @yield(i32 %n.val)
	  %save = call token @llvm.experimental.coro.save(i32 1)
	  %suspend = call i1 @llvm.experimental.coro.suspend(token %save)
	  br i1 %suspend, label %resume, label %cleanup
	
	resume:
	  %inc = add i32 %n.val, 1
	  br label %coro.start
	
	cleanup:
	  %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
	  call void @free(i8* %mem)
	  call void @llvm.experimental.coro.resume.end()  
	  br label %coro.return
	
	coro.return:
	  ret i8* %frame
	}
	
	;;;;;;;;;;;;;;;;;;;;;;;;;;;
	
	define i32 @main() {
	entry:
	  %hdl = call i8* @f(i32 4)
	  call void @llvm.experimental.coro.resume(i8* %hdl)
	  call void @llvm.experimental.coro.resume(i8* %hdl)
	  call void @llvm.experimental.coro.destroy(i8* %hdl)
	  ret i32 0
	}
