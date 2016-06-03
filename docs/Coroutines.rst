=====================================
Coroutines in LLVM
=====================================

.. contents::
   :local:
   :depth: 3

Status
======

This document describes a set of experimental extensions to LLVM. Use
with caution.  Because the intrinsics have experimental status,
compatibility across LLVM releases is not guaranteed. These intrinsics
are added to support C++ Coroutine TS (P0057), though they are general enough 
to be used to implement coroutines in other languages as well.

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

Coroutine Representation
------------------------

Let's look at an example of an LLVM coroutine with the behavior sketched
by the following pseudo-code.

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       yield(n++);
       <suspend> // magic: returns coroutine handle on first suspend
     }
  }

This coroutine calls some function `yield` with value `n` as an argument and
suspends execution. Every time it resumes it calls `yield` again with an 
argument one bigger than the last time. This coroutine never completes by 
itself and must be destroyed explicitly. If we use this coroutine with 
a `main` shown in the previous section. It will call `yield` with values 4, 5 
and 6 after which the coroutine will be destroyed.

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

The `coro.return` block returns a pointer to coroutine frame which happens to
be the same as `coroutine frame`_ expected by `coro.resume`_ and `coro.destroy`_
intrinsics.

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

In the coroutine shown in the previous section, use of virtual register `%n.val`
is separated from the definition by a suspend point, it cannot reside
on the stack frame of the coroutine since it will go away once coroutine is
suspended and therefore need to go into the coroutine frame.

Other members of the coroutine frame will be an address of a resume and destroy
functions representing the coroutine behavior that needs to happen when coroutine
is resumed and destroyed respectively.

.. code-block:: llvm

  %f.frame = type { void (%f.frame*)*, void (%f.frame*)*, i32 }

After coroutine transformation function `f` is responsible for creation and
initialization of the coroutine frame and execution of the coroutine code until
any suspend point is reached or control reaches the end of the function. It will
look like:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %alloc = call noalias i8* @malloc(i32 24)
    %0 = call nonnull i8* @llvm.experimental.coro.init(i8* %alloc, i8* null, i8* null)
    %frame = bitcast i8* %frame to %f.frame*
    %1 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 0
    store void (%f.frame*)* @f.resume, void (%f.frame*)** %1
    %2 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 1
    store void (%f.frame*)* @f.destroy, void (%f.frame*)** %2
   
    %n.val.addr = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 2
    store i32 %n, i32* %n.val.addr
    call void @yield(i32 %n)
   
    ret i8* %frame
  }

Part of the orginal coroutine `f` that is responsible for executing code after 
resume will be extracted into `f.resume` function:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* %frame.ptr.resume) {
  entry:
    %n.val.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %n.val = load i32, i32* %n.val.addr, align 4
    %inc = add i32 %n.val, 1
    store i32 %inc, i32* %n.val.addr, align 4
    tail call void @yield(i32 %inc)
    ret void
  }

Whereas function `f.destroy` will end up simply calling `free` function:

.. code-block:: llvm

  define internal fastcc void @f.destroy(%f.frame* %frame.ptr.destroy) {
  entry:
    %0 = bitcast %f.frame* %frame.ptr.destroy to i8*
    tail call void @free(i8* %0)
    ret void
  }

This transformation is performed by `coro-split` LLVM pass.

Avoiding Heap Allocations
-------------------------
 
A particular coroutine usage pattern which is illustrated by the `main` function
in the overview section where a coroutine is created, manipulated and destroyed by
the same calling function is common for generator coroutines and is suitable for
allocation elision optimization which stores coroutine frame in the caller's 
frame.

To enable heap elision, we need to make frame allocation and deallocation 
as follows:

In the entry block, we will invoke `coro.elide`_ intrinsic that will return 
an address of a coroutine frame on the callers if possible and `null` otherwise:

.. code-block:: llvm

  entry:
    %elide = call i8* @llvm.experimental.coro.elide()
    %0 = icmp ne i8* %elide, null
    br i1 %0, label %coro.init, label %coro.alloc

  coro.alloc:
    %frame.size = call i32 @llvm.experimental.coro.size()
    %alloc = call i8* @malloc(i32 %frame.size)
    br label %coro.init

  coro.init:
    %phi = phi i8* [ %elide, %entry ], [ %alloc, %coro.alloc ]
    %frame = call i8* @llvm.experimental.coro.init(i8* %phi, i8* null, i8* null)

In the cleanup block, we will make freeing the coroutine frame conditional on
`coro.delete`_ intrinsic. If allocation is elided, `coro.delete`_ returns `null`
thus avoiding deallocation code:

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    %tobool = icmp ne i8* %mem, null
    br i1 %tobool, label %if.then, label %if.end

  if.then:
    call void @free(i8* %mem)
    br label %if.end

  if.end:
    call void @llvm.experimental.coro.resume.end()
    br label %coro.return

With allocations and deallocations described as above after inlining and heap
allocation elision optimization the resulting main will end up looking as:

.. code-block:: llvm

  define i32 @main() {
  entry:
    call void @yield(i32 4)
    call void @yield(i32 5)
    call void @yield(i32 6)
    ret i32 0
  }

Multiple Suspend Points
-----------------------

Let's consider the coroutine that has more than one suspend point:

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       yield(n++);
       <suspend>
       yield(-n);
       <suspend>
     }
  }

Matching LLVM code would look like (with the rest of the code remaining the same
as the code in the previous section):

.. code-block:: llvm

  coro.start:
      %n.val = phi i32 [ %n, %coro.init ], [ %inc, %resume ]
      call void @yield(i32 %n.val)
      %suspend1 = call i1 @llvm.experimental.coro.suspend(token none)
      br i1 %suspend1, label %resume, label %cleanup

    resume:
      %inc = add i32 %n.val, 1
      %sub = sub nsw i32 0, %inc
      call void @yield(i32 %sub)
      %suspend2 = call i1 @llvm.experimental.coro.suspend(token none)
      br i1 %suspend2, label %coro.start, label %cleanup

In this case, coroutine frame would include a suspend index that will indicate
at which suspend point a coroutine needs to resume and `f.resume` function
will start with a switch as follows:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* nocapture nonnull %frame.ptr.resume) {
  entry:
    %index.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %index = load i32, i32* %0, align 4
    %switch = icmp eq i32 %index, 1
    br i1 %switch, label %resume, label %coro.start

  coro.start:
    ...
    br label %exit

  resume:
    ...
    br label %exit

  exit:
   %storemerge = phi i32 [ 2, %resume ], [ 1, %coro.start ]
    store i32 %storemerge, i32* %index.addr, align 4
    ret void
  }

If different cleanup code needs to get executed for different suspend points, 
a similar switch will be in the `f.destroy` function.

.. note ::

  Using suspend index in a coroutine state and having a switch in `f.resume` and
  `f.destroy` is one of the possible implementation strategies. We explored 
  another option where a distinct `f.resume1`, `f.resume2`, etc are created for
  every suspend point and instead of storing an index, the resume and destroy 
  function pointers are updated at every suspend. Early testing showed that the
  former is easier on the optimizer than the latter so it is a strategy 
  implemented at the moment.

Distinct Save and Suspend
-------------------------

In the previous example, setting a resume index (or some other state change that 
needs to happen to prepare coroutine for resumption) happens at the same time as
suspension of a coroutine. However, in certain cases it is necessary to control 
when coroutine is prepared for resumption and when it is suspended.

In the following example, coroutine represents some activity that is driven
by completions of asynchronous operations `async_op1` and `async_op2` which get
a coroutine handle as a parameter and will resume the coroutine once async
operation is finished.

.. code-block:: llvm

  void g() {
     for (;;)
       if (cond()) {
          async_op1(<coroutine-handle>); // will resume once async_op1 completes
          <suspend>
          do_one();
       }
       else {
          async_op2(<coroutine-handle>); // will resume once async_op2 completes
          <suspend>
          do_two();
       }
     }
  }

In this case, coroutine should be ready for resumption prior to a call to 
`async_op1` and `async_op2`. The `coro.save`_ intrinsic is used to indicate a
point when coroutine should be ready for resumption:

.. code-block:: llvm

  if.true:
    %save1 = call token @llvm.experimental.coro.save(i32 1)
    call void async_op1(i8* %frame)
    %suspend1 = call i1 @llvm.experimental.coro.suspend(token %save1)
    br i1 %suspend1, label %resume1, label %cleanup

  if.false:
    %save2 = call token @llvm.experimental.coro.save(i32 2)
    call void async_op2(i8* %frame)
    %suspend2 = call i1 @llvm.experimental.coro.suspend(token %save2)
    br i1 %suspend2, label %resume2, label %cleanup

Final Suspend
-------------

..Coroutines we considered so far do not complete on their own. They run
..until explicitly destroyed by the call to `coro.destroy`_. If we consider a case
..of a coroutine representing a generator that produces a finite sequence of

One of the common coroutine usage patterns is a generator, where a coroutine
produces a (sometime finite) sequence of values. To facilitate this pattern
frontend can designate a suspend point to be final. A coroutine suspended at
the final suspend point, can only be resumed with `coro.destroy`_ intrinsic.
Resuming such coroutine with `coro.resume`_ results in undefined behavior.
The `coro.done`_ intrinsic can be used to check whether a suspended coroutine
is at the final suspend point or not.

Reaching Inside
---------------

Coroutine author / front-end may designate a distinguished `alloca` that can be
used to communicate with the coroutine.

.. code-block:: llvm

  sdfsdf sdf sdf 
  sdfs dsdf sd fds 

Terms
=====

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

Coroutine Manipulation Intrinsics
---------------------------------

Intrinsics described in this section are used to manipulate an existing
coroutine. As such they can be used inside of any function that has access
to the coroutine handle.

.. _coro.destroy:

'llvm.experimental.coro.destroy' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Syntax:
"""""""

::

      declare void @llvm.experimental.coro.destroy(i8* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.destroy``' intrinsic destroys the suspended
coroutine.

Arguments:
""""""""""

The argument is a coroutine handle to a suspended coroutine.

Semantics:
""""""""""

The '``llvm.va_start``' intrinsic works just like the ``va_start`` macro
available in C. In a target-dependent way, it initializes the
``va_list`` element to which the argument points, so that the next call
to ``va_arg`` will produce the first variable argument passed to the
function. Unlike the C ``va_start`` macro, this intrinsic does not need
to know the last argument of the function as the compiler can figure
that out.

.. _coro.resume:

'llvm.experimental.coro.resume' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.done:

'llvm.experimental.coro.done' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.promise:

'llvm.experimental.coro.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.from.promise:

'llvm.experimental.coro.from.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla


Coroutine Structure Intrinsics
------------------------------
Intrinsics described in this section are used within a coroutine to describe
the coroutine structure. 

.. _coro.delete:

'llvm.experimental.coro.delete' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla


.. _coro.size:

'llvm.experimental.coro.size' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.init:

'llvm.experimental.coro.init' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.fork:

'llvm.experimental.coro.fork' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.resume.end:

'llvm.experimental.coro.resume.end' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.suspend:

'llvm.experimental.coro.suspend' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.save:

'llvm.experimental.coro.save' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

.. _coro.elide:

'llvm.experimental.coro.elide' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bla bla

Coroutine Transformation Passes
===============================
CoroEarly
---------
The pass CoroEarly lowers coroutine intrinsics that hide the details of the
structure of the coroutine frame, but, otherwise not needed to be preserved to
help later coroutine passes. This pass lowers `coro.done`_, `coro.promise`_ and
`coro.from.promise`_ intrinsics.

CoroInline
----------
Since coroutine transformation need to be done in the IPO order and inlining
pre-split coroutine is undesirable, the CoroInline pass wraps the inliner pass
to execute CoroElide pass on a pre-split coroutine, followe

CoroSplit
---------
The pass CoroSplit splits the coroutine into the start, resume and destroy parts.

CoroElide
---------


CoroLater
---------


Problem Areas
=============
#. Debug information is not supported at the moment
#. Coroutine frame is much bigger than it should. Stack packing and stack 
coloring like optimization performed on the coroutine frame will result in leaner
coroutine frame.