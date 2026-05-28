.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

====================
Mongoose daemon mode
====================

Introduction
------------

Mongoose is a daemon mode of SWUpdate that provides a web server, web
interface and web application.

The web application in ``web-app`` uses the `Node.js`_ package manager
and `gulp`_ as build tool. It depends on `Bootstrap 4`_,
`Font Awesome 5`_ and `Dropzone.js`_.

.. _Node.js: https://nodejs.org/en/
.. _gulp: https://gulpjs.com/
.. _Bootstrap 4: https://getbootstrap.com/
.. _Font Awesome 5: https://fontawesome.com/
.. _Dropzone.js: http://www.dropzonejs.com/


Startup
-------

After having configured and compiled SWUpdate with enabled mongoose web
server:

.. code:: bash

  ./swupdate --help

lists the mandatory and optional arguments to be provided to mongoose.
As an example,

.. code:: bash

  ./swupdate -l 5 -w '-r ./examples/www/v2 -p 8080' -p 'reboot'

runs SWUpdate in mongoose daemon mode with log-level ``TRACE`` and a web
server at http://localhost:8080.


Example
-------

The ready to use example of the web application in  the
``examples/www/v2`` directory uses a Public Domain `background.jpg`
image from `pixabay`_ with is released under the Creative Commons CC0
license. The used `favicon.png` and `logo.png` images are made from the
SWUpdate logo and therefore subject to the GNU General Public License
version 2. You must comply to this license or replace the images with
your own files.

.. _pixabay: https://pixabay.com/de/leiterbahn-platine-technologie-3157431/


Customize
---------

You could customize the web application inside the ``web-app`` directory.
Beside the replace of the `favicon.png`, `logo.png` and `background.jpg`
images inside the ``images`` directory you could customize the Bootstrap
colors and settings inside the ``scss/bootstrap.scss`` style sheet. The
style sheet changes need a rebuild of the web application source code.


Develop
-------

The development requires Node.js version 18 or greater and a prebuilt
SWUpdate project with enabled mongoose web server and web application
interface version 2 support.

#. Enter the web application directory::

    cd ./web-app

#. Install the dependencies::

    npm install

#. Build the web application::

    npm run build

#. Start the web application::

    ../swupdate -w '-r ./dist -p 8080' -p 'echo reboot'

#. Test the web application:

    http://localhost:8080/

#. Pack the web application (optional)::

    npm run package -- --output swupdate-www.tar.gz


Contribute
----------

Please run the linter before any commit

.. code:: bash

    npm run lint
