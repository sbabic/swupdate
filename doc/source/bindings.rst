.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

=============================================
Language Bindings
=============================================

Overview
--------

In general, SWUpdate is agnostic to a particular language it is operated from,
thanks to :doc:`SWUpdate's socket-based control <swupdate-ipc>` and
:doc:`progress APIs <progress>` for external programs. As long as the language
of choice has proper socket (library) support, SWUpdate can be operated with it.

However, for convenience, a Lua language binding in terms of a shared library,
currently ``lua_swupdate.so.0.1``, is provided.


Lua Language Binding
--------------------

The Lua language binding is realized in terms of the ``lua_swupdate`` module
that defines three bindings, namely for the control interface, the progress
interface, and a convenience function yielding a table holding all local
network interfaces including their IP addresses and submasks.

The ``lua_swupdate`` Lua module interface specification that details what
functionality is made available by ``bindings/lua_swupdate.c`` is found
in ``bindings/lua_swupdate.lua``. It serves as reference, for mocking
purposes, and type checking thanks to the EmmyLua-inspired annotations.

Note that, depending on the filesystem location of the Lua binding's shared
library, Lua's ``package.cpath`` may have to be adapted by setting the
environment variable ``LUA_CPATH``, modifying ``package.cpath`` prior to
a ``require('lua_swupdate')``, or , as last resort, using ``package.loadlib()``
instead of ``require('lua_swupdate')``.


Control Interface
.................

The ``lua_swupdate`` module's control interface binding conveniently makes
:doc:`SWUpdate's socket-based control API <swupdate-ipc>` available to pure Lua.

The binding is captured in the ``swupdate_control`` object that is returned
by a call to ``swupdate.control()``. This object offers the three methods
``connect()``, ``write(<chunkdata>)``, and ``close()``:

The ``connect()`` method initializes the connection to SWUpdate's control
socket, sends ``REQ_INSTALL``, and waits for ``ACK`` or ``NACK``, returning the
socket connection file descriptor, mostly for information purposes, or, in case
of an error, ``nil`` plus an error message.

The artifact's data can then be sent to SWUpdate via the ``write(<chunkdata>)``
method, returning ``true``, or, in case of errors, ``nil`` plus an error message.

Finally, the ``close()`` method closes the connection to SWUpdate's control
socket after which it waits for SWUpdate to complete the update transaction and
executes the post-install command, if given.

The following example snippet illustrates how to use the control interface binding:

::

	local artifact = io.open("/some/path/to/artifact.swu", "rb" )
	swupdate = require('lua_swupdate')
	local ctrl = swupdate.control()
	if not ctrl:connect() then
		-- Deliberately neglecting error message.
		io.stderr:write("Error connecting to SWUpdate control socket.\n")
		return
	end

	while true do
		local chunk = artifact:read(1024)
		if not chunk then break end
		if not ctrl:write(chunk) then
			-- Deliberately neglecting error message.
			io.stderr:write("Error writing to SWUpdate control socket.\n")
			break
		end
	end

	local res, msg = ctrl:close()
	if not res then
		io.stderr:write(string.format("Error finalizing update: %s\n", msg))
	end


Progress Interface
..................

The ``lua_swupdate`` module's progress interface binding conveniently makes
:doc:`SWUpdate's socket-based progress API <progress>` available to pure Lua.

The binding is captured in the ``swupdate_progress`` object that is returned
by a call to ``swupdate.progress()``. This object offers the three methods
``connect()``, ``receive()``, and ``close()``:

The ``connect()`` method connects to SWUpdate's progress socket, waiting until
the connection has been established. Note that it is only really required to
explicitly call ``connect()`` to reestablish a broken connection as the
``swupdate_progress`` object's instantiation already initiates the connection.

The ``receive()`` method returns a table representation of the ``struct
progress_msg`` described in the :doc:`progress interface's API description
<progress>`.

The ``close()`` method deliberately closes the connection to SWUpdate's progress
socket.


IPv4 Interface
..............

For convenience, the ``lua_swupdate`` module provides the ``ipv4()`` method
returning a table holding the local network interfaces as the table's keys and
their space-separated IP addresses plus subnet masks as respective values.
