/*!
 * Copyright (C) 2017-2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
 *
 * SPDX-License-Identifier: MIT
 */

import cleanCSS from 'gulp-clean-css'
import { deleteAsync } from 'del'
import gulp from 'gulp'
import gzip from 'gulp-gzip'
import imagemin from 'gulp-imagemin'
import filter from 'gulp-filter'
import htmlmin from 'gulp-htmlmin'
import minify from 'gulp-minify'
import rename from 'gulp-rename'
import replace from 'gulp-replace'
import * as dartSass from 'sass'
import gulpSass from 'gulp-sass'
import tar from 'gulp-tar'
import useref from 'gulp-useref'
import minimist from 'minimist'
const sass = gulpSass(dartSass)

const knownOptions = {
  string: 'output',
  default: { output: 'swupdate-www' }
}

const options = minimist(process.argv.slice(2), knownOptions)

gulp.task('sass', async function () {
  return gulp.src('scss/*.scss')
    .pipe(sass().on('error', sass.logError))
    .pipe(cleanCSS({ compatibility: '*' }))
    .pipe(rename({
      suffix: '.min'
    }))
    .pipe(gulp.dest('dist/css'))
})

gulp.task('minify-css', async function () {
  return gulp.src('css/*.css')
    .pipe(cleanCSS({ compatibility: '*' }))
    .pipe(rename({
      suffix: '.min'
    }))
    .pipe(gulp.dest('dist/css'))
})

gulp.task('minify-js', async function () {
  return gulp.src('js/*.js')
    .pipe(minify({
      ext: {
        min: '.min.js'
      },
      noSource: true,
      preserveComments: 'some'
    }))
    .pipe(gulp.dest('dist/js'))
})

gulp.task('minify-html', async function () {
  return gulp.src('*.html')
    .pipe(useref({ noconcat: true }))
    .pipe(filter('*.html'))
    .pipe(replace('vendor/bootstrap/bootstrap.min.css', 'css/bootstrap.min.css'))
    .pipe(replace('vendor/dropzone/dropzone.min.css', 'css/dropzone.min.css'))
    .pipe(replace('vendor/fontawesome/all.min.css', 'css/fontawesome.min.css'))
    .pipe(replace('vendor/bootstrap/bootstrap.bundle.min.js', 'js/bootstrap.bundle.min.js'))
    .pipe(replace('vendor/dropzone/dropzone.min.js', 'js/dropzone.min.js'))
    .pipe(replace('vendor/jquery/jquery-3.7.1.min.js', 'js/jquery.min.js'))
    .pipe(htmlmin({ collapseWhitespace: true }))
    .pipe(gulp.dest('dist'))
})

gulp.task('copy-css', async function () {
  return gulp.src('*.html')
    .pipe(useref({ noconcat: true }))
    .pipe(filter('**/*.css'))
    .pipe(rename({
      dirname: 'css',
      suffix: '.min'
    }))
    .pipe(cleanCSS({ compatibility: '*' }))
    .pipe(gulp.dest('dist'))
})

gulp.task('copy-js', async function () {
  return gulp.src('*.html')
    .pipe(useref({ noconcat: true }))
    .pipe(filter('**/*.js'))
    .pipe(rename({
      dirname: 'js'
    }))
    .pipe(minify({
      ext: {
        min: '.min.js'
      },
      noSource: true,
      preserveComments: 'some'
    }))
    .pipe(gulp.dest('dist'))
})

gulp.task('copy-fonts', async function () {
  return gulp.src([
    'vendor/fontawesome/webfonts/*'
  ])
  .pipe(gulp.dest('dist/webfonts'))
})

gulp.task('resize-images', async function () {
  return gulp.src('images/*')
    .pipe(imagemin({ verbose: true }))
    .pipe(gulp.dest('dist/images'))
})

gulp.task('package', function () {
  const name = options.output.replace('.tar', '').replace('.gz', '')
  return gulp.src('dist/**')
    .pipe(tar(name + '.tar'))
    .pipe(gzip())
    .pipe(gulp.dest('.'))
})

gulp.task('clean', function () {
  return deleteAsync(['dist/**'])
})

gulp.task('build', gulp.series('clean', gulp.parallel('copy-css', 'copy-js', 'copy-fonts', 'sass', 'minify-css', 'minify-js', 'minify-html', 'resize-images')))

gulp.task('default', gulp.series('build'))
