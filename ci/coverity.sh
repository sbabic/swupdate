#!/bin/sh
# Copyright (c) Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:	GPL-2.0-only

set -eu
curl -o /tmp/cov-analysis-linux64.tgz https://scan.coverity.com/download/linux64 --form project=$COVERITY_SCAN_PROJECT_NAME --form token=$COVERITY_SCAN_TOKEN
tar xfz /tmp/cov-analysis-linux64.tgz
make all_handlers_defconfig
cov-analysis-linux64-*/bin/cov-build --dir cov-int make -j 8
tar cfz cov-int.tar.gz cov-int
curl https://scan.coverity.com/builds?project=$COVERITY_SCAN_PROJECT_NAME --form token=$COVERITY_SCAN_TOKEN --form email=$GITLAB_USER_EMAIL --form file=@cov-int.tar.gz --form version="`git describe --tags`" --form description="`git describe --tags` / $CI_COMMIT_TITLE / $CI_COMMIT_REF_NAME:$CI_PIPELINE_ID "
