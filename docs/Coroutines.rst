=====================================
Coroutines in LLVM
=====================================

.. contents::
   :local:
   :depth: 2

Status
======

This document describes a set of experimental extensions to LLVM. Use
with caution.  Because the intrinsics have experimental status,
compatibility across LLVM releases is not guaranteed.

Overview
========

LLVM coroutines are functions that have one or more `suspend points`_. 
When a suspend point is reached, execution of a coroutine is suspended. 
A suspended coroutine can be resumed to continue execution from the last 
suspend point or be destroyed. In the following example function `f` returns
a handle to a suspended coroutine (`coroutine handle`_) that can be passed to 
`coro.resume`_ and `coro.destroy`_ intrinsics to resume and destroy the 
coroutine respectively.

.. code-block:: llvm

  define i32 @main() {
  entry:
    %hdl = call i8* @f(i32 4)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    call void @llvm.experimental.coro.destroy(i8* %hdl)
    ret i32 0
  }

In addition to the stack frame which exists when a coroutine is executing, 
there is an additional region of storage that contains objects that keeps the 
coroutine state when a coroutine is suspended. This region of storage
is called `coroutine frame`_. It is created when a coroutine is invoked.
It is destroyed when a coroutine runs to completion or destroyed. 

An LLVM coroutine is represented as an LLVM function with calls to set of 
coroutine specific intrinsics marking up suspend points and coroutine frame 
allocation and deallocation code. Marking up allocation and deallocation code 
allows an optimization to remove allocation/deallocation when coroutine frame
can be stored on a frame of the caller. 

After mandatory coroutine processing passes a coroutine is split into several
functions that represent three different way of how control can enter the 
coroutine: initial invocation that creates the coroutine frame and executes
the coroutine code until it encounters first suspend point or reaches the end
of the coroutine, coroutine resume function that contains the code to be 
executed once coroutine is resumed at a particular suspend point, and a 
coroutine destroy function that is invoked when coroutine is destroyed.

Coroutines by Example
=====================

Coroutine representation
------------------------

Let's look at an example of an LLVM coroutine with the behavior sketched
by the following pseudo-code.

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       yield(n++);
       <suspend> // magic: returns coroutine handle of first suspend
     }
  }

This coroutine calls some function `yield` with value `n` as an argument and
suspends execution. Every time it resumes it calls `yield` again with an 
argument one bigger than the last time. This coroutine never completes by 
itself and must be destroyed explicitly. If we use this coroutine with 
a `main` shown in the previous section. It will call `yield` with values 4 and 5
after which the coroutine will be destroyed.

We will look at individual parts of the LLVM coroutine matching the pseudo-code
above starting with coroutine frame creating and destruction:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %frame.size = call i32 @llvm.experimental.coro.size()
    %alloc = call i8* @malloc(i32 %frame.size)
    %frame = call i8* @llvm.experimental.coro.init(i8* %alloc, i8* null, i8* null)
    %first.return = call i1 @llvm.experimental.coro.fork()
    br i1 %first.return, label %coro.return, label %coro.start
  
  coro.start:
    ; ...
  resume:
    ; ...

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    call void @free(i8* %mem)
    call void @llvm.experimental.coro.resume.end()  
    br label %coro.return

  coro.return:
    ret i8* %frame
  }

First three lines of `entry` block establish the coroutine frame. The
`coro.size`_ intrinsic expands to represent the size required for the coroutine
frame. The `coro.init`_ intrinsic returns the address to be used as a coroutine
frame pointer (which could be at offset relative to the allocated block of
memory). We will examine the other two parameters to `coro.init`_ later.

In the cleanup block `coro.delete` intrinsic, given the coroutine frame pointer,
returns a memory address to be freed.

Two other intrinsics seen in this fragment are used to mark up the control flow
during the initial and subsequent invocation of the coroutine. The true branch
of the conditional branch following the `coro.fork`_ intrinsic indicates the 
block where control flow should transfer on the first suspension of the
coroutine or if control reaches the end of the function without encountering 
any suspend points. The `coro.resume.end`_ intrinsic is a no-op during an 
initial invocation of the coroutine. When the coroutine resumes, the intrinsic
marks the point when coroutine need to return control back to the caller.

.. The `malloc` function is used to allocate memory dynamically for 
.. coroutine frame.   

The rest of the coroutine code in blocks `coro.start` and `resume` 
is straightforward:

.. code-block:: llvm

  coro.start:
    %n.val = phi i32 [ %n, %entry ], [ %inc, %resume ]
    call void @yield(i32 %n.val)
    %suspend = call i1 @llvm.experimental.coro.suspend(token none)
    br i1 %suspend, label %resume, label %cleanup

  resume:
    %inc = add i32 %n.val, 1
    br label %coro.start

When control reaches `coro.suspend`_ intrinsic, the coroutine is suspended.
The conditional branch following the `coro.suspend` intrinsic indicates two
alternative continuation for the coroutine, one for normal resume, another
for destroy.

Coroutine Transformation
------------------------

Terms
=====
**Coroutine Handle**
  a pointer that encodes information about an



High Level Structure
====================

.. _suspend point:
.. _suspend points:

Suspend point
-------------
bla bla

.. _coroutine frame:

Coroutine frame
---------------
bla bla

.. _coroutine handle:

Coroutine Handle
----------------
bla bla

Intrinsics
==========

.. _coro.destroy:

'llvm.experimental.coro.destroy' Intrinsic
------------------------------------------
bla bla

.. _coro.resume:

'llvm.experimental.coro.resume' Intrinsic
-----------------------------------------
bla bla

.. _coro.size:

'llvm.experimental.coro.size' Intrinsic
---------------------------------------
bla bla

.. _coro.init:

'llvm.experimental.coro.init' Intrinsic
---------------------------------------
bla bla

.. _coro.fork:

'llvm.experimental.coro.fork' Intrinsic
---------------------------------------
bla bla

.. _coro.resume.end:

'llvm.experimental.coro.resume.end' Intrinsic
---------------------------------------------
bla bla

.. _coro.suspend:

'llvm.experimental.coro.suspend' Intrinsic
---------------------------------------------
bla bla

.. _coro.save:

'llvm.experimental.coro.save' Intrinsic
---------------------------------------------
bla bla
