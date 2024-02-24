/*!
 * Copyright (C) 2017-2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
 *
 * SPDX-License-Identifier: MIT
 */

const cleanCSS = require('gulp-clean-css')
const del = require('del')
const gulp = require('gulp')
const gzip = require('gulp-gzip')
const imagemin = require('gulp-imagemin')
const filter = require('gulp-filter')
const htmlmin = require('gulp-htmlmin')
const minify = require('gulp-minify')
const rename = require('gulp-rename')
const replace = require('gulp-replace')
const sass = require('gulp-sass')(require('sass'))
const tar = require('gulp-tar')
const useref = require('gulp-useref')
const minimist = require('minimist')

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
    .pipe(replace('node_modules/bootstrap/dist/css', 'css'))
    .pipe(useref({ noconcat: true }))
    .pipe(filter('*.html'))
    .pipe(replace(/node_modules\/.*\/([^/]+)\.css/g, 'css/$1.css'))
    .pipe(replace(/node_modules\/.*\/([^/]+)\.js/g, 'js/$1.js'))
    .pipe(replace('.css', '.min.css'))
    .pipe(replace('.js', '.min.js'))
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
    'node_modules/@fortawesome/fontawesome-free/webfonts/fa-solid-900.{ttf,woff,woff2}'
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
  return del('dist/**', { force: true })
})

gulp.task('build', gulp.series('clean', gulp.parallel('copy-css', 'copy-js', 'copy-fonts', 'sass', 'minify-css', 'minify-js', 'minify-html', 'resize-images')))

gulp.task('default', gulp.series('build'))
