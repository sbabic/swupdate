..
        SPDX-FileCopyrightText: 2021 Stefano Babic <stefano.babic@swupdate.org>
        SPDX-License-Identifier: GPL-2.0-only

==========================
Delta Update with SWUpdate
==========================

Overview
--------

The size of update packages is steadily increasing. While once the whole software was just
a bunch of megabytes, it is not unusual now that OS and application on devices running
Linux as OS reach huge size of Gigabytes.

Several mechanisms can be used to reduce the size of downloaded data. The resulting images
can be compressed. However, this is not enough when bandwidth is important and not cheap.
It is very common that a device will be upgraded to a version that is similar to the
running one but add new features and solves some bugs. Specially in case of just fixes,
the new version is pretty much equal as the original one. This asks to find methods to
download just the differences with the current software without downloading a full image.
In case an update is performed from a known base, we talk about *delta updates*. In the following
chapter some well known algorithms are considered and verified if they can be integrated
into SWUpdate. The following criteria are important to find a suitable algorithm:

        - license must be compatible with GPLv2
        - good performance for smaller downloads, but not necessarily the best one.
        - SWUpdate remains with the concept to deliver one package (SWU), the same
          independently from the source where the SWU is stored (USB, OTA, etc.)
        - It must comply to SWUpdate's security requirements (signed images, privilege separation, etc.)

Specific ad-hoc delta updates mechanisms can be realized when the nature of the updated files is
the same. It is always possible with SWUpdate to install single files, but coherency and compatibility
with the runningsoftware must be guaranteed by the integratot / manufacturer. This is not covered here:
the scope is to get an efficient and content unaware *delta* mechanism, that can upgrade in differential
mode two arbitrary images, without any previous knowledge about what they content.

FOSS projects for delta encoding
--------------------------------

There are several algorithms for *delta encoding*, that is to find the difference between files,
generally in binary format. Only algorithms available under a compatible FOSS license (GPLv2) 
are considered for SWUpdate.
One of the goals in SWUpdate is that it should work independently which is the format of the
artifacts. Very specialized algorithm and libraries like Google's Courgette used in Chromium will give
much better results, but it works on programs (ELF files) and take advantages of the structure of compiled
code. In case of OTA update, not only software, but any kind of artifact can be delivered, and this includes
configuration data, databases, videos, docs, etc.

librsync_
.........

librsync_ is an independent implementation for rsync and does not use the rsync protocol. It is well
suited to generate offline differential update and it is already integrated into SWUpdate.
However, librsync takes the whole artifact and generates a differential image that is applied
on the whole image. It gives the best results in terms of reduced size when differences are
very small, but the differential output tends to be very large as soon as the differences
are meaningful. Differential images created for SWUpdate show that, as soon as the difference larger is,
the resulting delta image can even become larger as the original one.

SWUpdate supports `librsync` as delta encoder via the rdiff handler.

xdelta_
.......

xdelta_ uses the VCDIFF algorithm to compute differences between binaries. It is often used
to deliver smaller images for CD and DVD. The resulting images are created from an installed
image that should be loaded entirely in main memory. For this reason, it does not scale well
when the images are becoming larger and it is unsuitable for embedded systems and SWUpdate.

casync_
.......

casync_ is, according to his author. a tool for distributing images. It has several interesting
aspects that can be helpful with OTA update.
Files itself are grouped together in chunks and casync creates a "Chunk storage" where each chunk
is stored on a separate file. The chunk storage is part of the delivery, and it must be stored on
a server. casync checks if the chunk is already present on the target, and if not
download it. If this seems to be what is required, there are some drawbacks if casync
should be integrated in SWUpdate:

        - because of the nature of casync, each chunk is a separate file. This cause a huge
          number of new connections, because each file is a separate GET on the server.
          The overhead caused to re-instantiate connection is high on small devices,
          where SSL connections are also increasing CPU load. There are downloads of
          hundreds or thousands of small files just to recreate the original metadata
          file.
        - casync has no authentication and verification and the index (.caidx or .caibx)
          are not signed. This is known, but casync goals and scopes are outside the ones
          on embedded devices.
        - it is difficult to deliver a whole chunk storage. The common usage for OTA is to deliver
          artifacts, and they should be just a few. Thousands of files to be delivered to let
          casync to compute the new image is not practical for companies: they have a new "firmware"
          or "software" and they need an easy way to deliver this file (the output from their build system)
          to the devices. In some cases, they are even not responsible for that, and the firmware is given to
          another authority that groups all packages from vendors and realizes a sort of OTA service.
        - casync is quite a huge project - even if it was stated that it will be converted into
          a library, this never happened. This makes difficult to interface to SWUpdate,
          and using it as external process is a no way in SWUpdate for security reason.
          It breaks privilege separation, and adds a lot of code that is difficult
          to maintain.

For all these reasons, even if the idea of a chunk storage is good for an OTA updater, casync
is not a candidate for SWUpdate. A out-of-the-box solution cannot be found, and it is required to
implement an own solution that better suits for SWUpdate.

Zchunk_ - compression format
............................

zchunk_ seems to combine the usage of a chunk storage without having to deliver it on a server.
zchunk is a FOSS project released under BSD by its author_. The goal of this project is something else:
zchunk creates a new compression format that adds the ability to download the differences between
new and old file. This matches very well with SWUpdate. A zchunk file contains a header that
has metadata for all chunks, and according to the header, it is known which chunks must be
downloaded and which ones can be reused. zchunk has utilities to download itself the missing chunks,
but it could be just used to find which part of an artifact must be downloading,
and SWUpdate can go on with its own way to do this.

One big advantage on this approach is that metadata and compressed chunks are still bound into a single file,
that can be built by the buildsystem and delivered as it is used to. The updater needs first the metadata, that is
the header in zchunk file, and processes it to detect which chunks need to be downloaded. Each chunk has
its own hash, and the chunks already available on the device are verified against the hash to be sure
they are not corrupted.

Zchunk supports multiple sha algorithms - to be compatible with SWUpdate, zchunk should be informed
to generate sha256 hashes.

Design Delta Update in SWUpdate
-------------------------------

For all reasons stated before, `zchunk` is chosen as format to deliver delta update in SWUpdate.  An artifact
can be generated in ZCK format and then the ZCK's header (as described in format_) can be extracted and
added to the SWU. In this way, a ZCK file is signed (and if requested compressed and/or encrypted) as
part of the SWU, and loading chunks from an external URL can be verified as well because the corresponding
hashes are already verified as part of the header.


.. _casync: http://0pointer.net/blog/casync-a-tool-for-distributing-file-system-images.html
.. _xdelta: http://xdelta.org/
.. _zchunk: https://github.com/zchunk/zchunk
.. _author: https://www.jdieter.net/posts/2018/05/31/what-is-zchunk/ 
.. _librsync: https://librsync.github.io/
.. _format: https://github.com/zchunk/zchunk/blob/main/zchunk_format.txt

Changes in ZCHUNK project
-------------------------

Zchunk has an API that hides most of its internal, and provides a set of tools for creating
and downloading itself a file in ZCK format. Nevertheless, Zchunk relies on hashes for the compressed
(ZST) chunks, and it was missing for support for uncompressed data. To combine SWUpdate and zchunk,
it is required that a comparison can be done between uncompressed data, because it is unwanted that
a device is obliged to compress big amount of data just to perform a comparisons. 
A short list of changes in the Zchunk project is:

        - create hashes for uncompressed data and extend format to support it. The header
          must be extended to include both size and hash of uncompressed data.
        - make the library embedded friendly, that means reports errors in case of failure
          instead of exiting and find a suitable way to integrate the log output
          for the caller.
        - allow to use sha256 (already foreseen in zchunk) as this is the only hash type
          used in SWUpdate.
        - add API to allow an external caller to take itself the decision if a chunk must be
          downloaded or reused.

These changes were merged into Zchunk project - be sure to get a recent version of Zchunk, at least with 
commit 1b36f8b5e0ecb, that means newer as 1.1.16.

Most of missing features in Zchunk listed in TODO for the project have no relevance here:
SWUpdate already verifies the downloaded data, and there is no need to add signatures to Zchunk itself.

Integration in sw-description
-----------------------------

The most important part in a Zchunk file is the header: this contains all metadata and hashes to perform
comparisons. The `zck` tool splits a file in chunks and creates the header. Size of the header are know, and the
header itself can be extracted from the ZCK file.
The header will be part of sw-description: this is the header for the file that must be installed. Because the header
is very small compared to the size of the whole file (quite 1 %), this header can be delivered into the SWU.


Integration in SWUpdate: the delta handler
------------------------------------------

The delta handler is responsible to compute the differences and to download the missing parts. It is not responsible
to install the artifact, because this breaks the module design in SWUpdate and will constrain to have
just one artifact type, for example installing as `raw` or `rawfile`. But what about if the artifact should be installed
by a different handler, for example UBI, or a custom handler ?
The best way is that the delta handler does not install, but it creates the stream itself so that this stream
can be passed to another (chained) handler, that is responsible for installing. All current SWUpdate's handlers
can be reused: each handler does not know that the artifact is coming with separate chunks and it sees just a stream
as before.
The delta handler has in short the following duties:

        - parse and understanf the ZCK header
        - create a ZCK header from the file / partition used as source for the comparison
        - detect which chunks are missing and which one must be copied.
        - build  a mixer that copies and downloads all chunks and generates a stream
          for the following handler.
        - detect any error coming form the chained handler.

Because the delta handler requires to download more data, it must start a connection to the storage
where the original ZCK is stored. This can lead to security issues, because handlers run with high
privileges because they write into the hardware. In fact, this breaks `privilege separation` that is
part of SWUpdate design.
To avoid this, the delta handler does not download itself. A separate process, that can runs with different
userid and groupid, is responsible for this. The handler sends a request to this process with a list of
ranges that should be downloaded (see HTTP Range request). The delta handler does not know how the chunks are
downlaoded, and even if using HTTP Range Request is the most frequent choice, it is open to further
implementations.
The downloader process prepares the connection and asks the server for ranges. If the server is not
able to provide ranges, the update aborts. It is in fact a requirement for delta update that the
server storing the ZCK file is able to answer to HTTP Range Request, and there is no fallback to download
the full file.
An easy IPC is implemented between the delta handler and the downloader process. This allows to exchange
messages, and the downloader can inform the handler if any error occurs so that the update can be stopped.
The downloader will send a termination message when all chunks will be downloaded.
Because the number of missing chunks can be very high, the delta handler must sends and organize
several requests to the downloader, and tracking each of them.
The downloader is thought as dummy servant: it starts the connection, retrieves HTTP headers and data,
and sends them back to the caller. The delta handler is then responsible to parse the answer, and to
retrieve the missing chunks from the multipart HTTP body.

Creation of ZCK Header and ZCK file for SWUpdate
------------------------------------------------

Zchunk supports more SHA algorithms and it sets as default SHA512/128. This is not compatible with SWUpdate
that just support SHA256. Be sure to generate header and chunks with SHA256 support.
You have to enable the generation of hashes for uncompressed chunk, too. A possible usage of the tool
is:

::

        zck --output <output file> -u --chunk-hash-type sha256 <artifact, like rootfs>

The output is the ZCK file with all chunks.
This file should be put on a Webserver accessible to the target, and that supports Range Request
(RFC 7233). All modern Webserver support it.

The SWU must just contain the header. This can be easy extracted from the ZCK file with:

::

        HSIZE=`zck_read_header -v <ZCK file> | grep "Header size" | cut -d':' -f2`
        dd if=<ZCK FILE> of=<ZCK HEADER file> bs=1 count=$((HSIZE))

Using ZCK tools to foresee download size
----------------------------------------

There are tools that can be used at build time to know how many chunks should be downloaded
when a device is upgrading from a known version. You can use `zck_cmp_uncomp` from the test
directory:

::

        ../build/test/zck_cmp_uncomp --verbose <uncompressed old version file> <ZCK file>

This prints a list with all chunks, marking them with SRC if they are the same in the old version
and they should not retrieved and with DST if they are new and must be downloaded.
The tool show at the end a summary with the total number of bytes of the new release (uncompressed) and how
many bytes must be downloaded for the upgrade.
Please remmeber that these value are just payload. SWUpdate reports a summary, too, but it takes into account
also the HTTP overhead (headers, etc.), so that values are not the same and the ones from SWUpdate are
slightly bigger.
