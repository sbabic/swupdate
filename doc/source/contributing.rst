.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

Contributing to SWUpdate
========================

Contributions are welcome ! Please follow the following guideline for contributions.

Contribution Checklist
----------------------

These are mostly general recommendations and are common practice in a lot of
FOSS projects.

- use git to manage your changes [*recomended*]

- follow as much as possible kernel codestyle [*recomended*]
  Nevertheless, some rules are not so strict as in kernel. The maximum line length
  can be extended over 80 chars if this increase code readability.

- add the required copyright header to each new file introduced [**required**]

- add signed-off to all patches [**required**]
    - to certify the "Developer's Certificate of Origin", see below
    - check with your employer when not working on your own!

- add version number for your patches if follow-up versions are requested [*recomended*]
    - Add a "Change from Vx" description under the commit message to take track
      of the history of the patch.
    - It is suggested to use excellent "patman" tool to manage patches series.
      This is part of U-Boot's project (tools/patman), but it can be used in other projects, too.

- check that your patches do not break build [**required**]

  - There is a set of configuration files in the `configs/` directory.
    Please run a build for all files in the directory to ensure that SWUpdate is
    still buildable from configurations different as yours.

- post patches to mailing list [**required**]
    - use `git format-patch` to generate your patches.
    - use `git send-email` if possible. This avoid corruptions due
      to the mailers
    - add a prefix [meta-swupdate] if patches are intended to the Yocto's meta layer.
    - send patches inline, do not append them
    - no HTML emails!

- do not use github Pull Request. github facilities are not used for this project.
  The review is done in a single place : the Mailing List. PR from github are ignored.

Patches are tracked by patchwork (see http://jk.ozlabs.org/projects/patchwork/).
You can see the status of your patches at http://patchwork.ozlabs.org/project/swupdate/list.

Developer's Certificate of Origin 1.1
-------------------------------------

When signing-off a patch for this project like this

    Signed-off-by: Random J Developer <random@developer.example.org>

using your real name (no pseudonyms or anonymous contributions), you declare the
following:

    By making a contribution to this project, I certify that:

        (a) The contribution was created in whole or in part by me and I
            have the right to submit it under the open source license
            indicated in the file; or

        (b) The contribution is based upon previous work that, to the best
            of my knowledge, is covered under an appropriate open source
            license and I have the right under that license to submit that
            work with modifications, whether created in whole or in part
            by me, under the same open source license (unless I am
            permitted to submit under a different license), as indicated
            in the file; or

        (c) The contribution was provided directly to me by some other
            person who certified (a), (b) or (c) and I have not modified
            it.

        (d) I understand and agree that this project and the contribution
            are public and that a record of the contribution (including all
            personal information I submit with it, including my sign-off) is
            maintained indefinitely and may be redistributed consistent with
            this project or the open source license(s) involved.

