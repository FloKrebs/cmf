#!/usr/bin/env python

# Copyright 2010 by Philipp Kraft
# This file is part of cmf.
#
#   cmf is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   cmf is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with cmf.  If not, see <http://www.gnu.org/licenses/>.
#   

# This file can build and install cmf
from __future__ import print_function, division
import sys
import os
import io
import re
import time

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from distutils.sysconfig import customize_compiler
from distutils.command.build_py import build_py

version = '2.0.0a0'

branchversion = version
try:
    from pygit2 import Repository
    head = Repository('.').head.shorthand
    if head != 'master':
        branchversion = version + '.' + head
except:
    Repository = None

print('cmf', branchversion)

# Try to import numpy, if it fails we have a problem
try:
    # Import a function to get a path to the include directories of numpy
    # noinspection PyPackageRequirements
    from numpy import get_include as get_numpy_include
except ImportError:
    raise RuntimeError("For building and running of cmf an installation of numpy is needed")


swig = False
openmp = False
debug = False

class CmfBuildExt(build_ext):
    """
    Custom build class to get rid of the -Wstrict-prototypes warning
    source: https://stackoverflow.com/a/36293331/3032680

    Removes also CLASS_swigregister clutter in cmf_core.py
    """
    @staticmethod
    def clean_swigregister(cmf_core_py):
        """
        SWIG creates ugly CLASS_swigregister functions. We remove them.
        """
        rp_call = re.compile(r'^(\w*?)_swigregister\((\w*?)\)', re.MULTILINE)
        cmf_core_py, n = rp_call.subn('# \\1 end', cmf_core_py)
        print(n, 'CLASS_swigregister(CLASS) lines deleted')
        rp_def = re.compile(r'^(\w*?)_swigregister = _cmf_core\.(\w*?)_swigregister', re.MULTILINE)
        cmf_core_py, n = rp_def.subn('_cmf_core.\\1_swigregister(\\1)', cmf_core_py)
        print(n, 'CLASS_swigregister = _cmf_core... -> _cmf_core.CLASS_swigregister(CLASS)')
        return cmf_core_py

    @staticmethod
    def clean_static_methods(cmf_core_py):
        """SWIG creates still static methods as free functions (extra) to ensure Py2.2 compatibility
        We don't want that in 2018
        """
        # Find class names and free functions
        classes = re.findall(r'class\s(\w*?)\(.*?\):', cmf_core_py, re.MULTILINE)
        funcs = re.findall(r'^def (\w*?)\(\*args, \*\*kwargs\):$', cmf_core_py, flags=re.MULTILINE)

        # Find old style static methods (def CLASS_method():)
        static_methods = [f for f in funcs if [c for c in classes if f.startswith(c)]]

        count = 0
        for sm in static_methods:
            cmf_core_py, n = re.subn(r'^def {}.*?:.*?return.*?$'.format(sm),
                                     '\n\n', cmf_core_py, flags=re.MULTILINE + re.DOTALL)
            count += n
        print(count, 'old style static methods removed from', len(classes), 'classes')
        return cmf_core_py

    def build_extensions(self):
        customize_compiler(self.compiler)
        try:
            self.compiler.compiler_so.remove("-Wstrict-prototypes")
        except (AttributeError, ValueError):
            pass
        build_ext.build_extensions(self)

        if swig:
            if os.path.exists('cmf/cmf_core_src/cmf_core.py'):
                fn = 'cmf/cmf_core_src/cmf_core.py'
            elif os.path.exists('cmf/cmf_core.py'):
                fn = 'cmf/cmf_core.py'
            else:
                raise RuntimeError('cmf_core.py not found, run "python setup.py build_ext swig" to create it')
            cmf_core_py = open(fn).read()
            cmf_core_py = self.clean_swigregister(cmf_core_py)
            cmf_core_py = self.clean_static_methods(cmf_core_py)

            open('cmf/cmf_core.py', 'w').write(cmf_core_py)

            if os.path.exists('cmf/cmf_core_src/cmf_core.py'):
                os.unlink('cmf/cmf_core_src/cmf_core.py')


def updateversion():
    """
    Writes the actual revision number into the relevant files:
        cmf/__init__.py: set __version__ constant
        Doxyfile: set PROJECT_NUMBER
    """
    try:
        module_code = open('cmf/__init__.py').readlines()
    except IOError:
        pass
    else:
        fout = open('cmf/__init__.py', 'w')
        for line in module_code:
            if line.startswith('__version__'):
                fout.write("__version__ = '{}'\n".format(version))
            elif line.startswith('__compiletime__'):
                fout.write("__compiletime__ = '{}'\n".format(time.ctime()))
            else:
                fout.write(line)
    try:
        doxycode = open('tools/Doxyfile').readlines()
    except IOError:
        pass
    else:
        fout = open('tools/Doxyfile', 'w')
        for line in doxycode:
            if line.strip().startswith('PROJECT_NUMBER'):
                fout.write("PROJECT_NUMBER         = {}\n".format(version))
            else:
                fout.write(line)


def pop_arg(arg):
    """
    Looks for a commandline argument and removes it from syws.argv
    returns: True if the argument is present, False when it is missing
    """
    try:
        sys.argv.remove(arg)
        return True
    except ValueError:
        return False


def count_lines(files):
    """
    Python version of the bash command wc -l
    """
    lcount = 0
    for fn in files:
        if not fn.endswith('.c'):
            lines = open(fn, encoding='utf-8').readlines()
            lcount += len(lines)
    return lcount


def is_source_file(fn, include_headerfiles=False):
    """
    Returns True if fn is the path of a source file
    """
    fn = fn.lower()
    res = (
        (fn.endswith('.cpp') and 'apps' not in fn)
        or (include_headerfiles and fn.split('.')[-1] not in ('.h', '.hpp'))
    )
    return res


def get_source_files(include_headerfiles=False):
    cmf_files = []
    for root, _dirs, files in os.walk(os.path.join('cmf', 'cmf_core_src')):
        if os.path.basename(root) != 'debug_scripts':
            cmf_files.extend(
                os.path.join(root, f)
                for f in files
                if is_source_file(os.path.join(root, f), include_headerfiles) and f != 'cmf_wrap.cpp')
    return cmf_files


def make_cmf_core():
    """
    Puts all information needed for the Python extension object together
     - source files
     - include dirs
     - extra compiler flags
    """
    # Include CVODE
    include_dirs = ['lib/include', 'lib/include/suitesparse']
    # Include numpy
    include_dirs += [get_numpy_include()]

    static_libraries = [('lib/lib',
                         ['sundials_cvode', 'sundials_sunlinsolklu',
                          'klu', 'amd', 'btf', 'colamd', 'suitesparseconfig']),
                        ]
    library_dirs = []
    libraries = []

    extra_objects = []
    # Platform specific stuff, alternative is to subclass build_ext command as in:
    # https://stackoverflow.com/a/5192738/3032680
    if sys.platform == 'win32':

        compile_args = ["/EHsc",
                        r'/Fd"build\vc90.pdb"',
                        "/D_SCL_SECURE_NO_WARNINGS",
                        "/D_CRT_SECURE_NO_WARNINGS",
                        "/MP"
                        ]
        if openmp:
            compile_args.append("/openmp")

        if debug:
            link_args = ["/DEBUG"]

        # Move static libraries to libraries, because MSVC does not
        # seperate between dynamic and static libraries at this point
        for lib_dir, libs in static_libraries:
            checked_libs=[]
            for lib in libs:
                if os.path.exists(os.path.join(lib_dir, lib + '.lib')):
                    checked_libs.append(lib)
                elif os.path.exists(os.path.join(lib_dir, 'lib' + lib + '.lib')):
                    checked_libs.append('lib' + lib)
                else:
                    raise FileNotFoundError("Can't find static library " + os.path.join(lib_dir, lib))
            libraries.extend(checked_libs)
            library_dirs.append(lib_dir)

    else:

        compile_args = ['-Wno-comment', '-Wno-reorder', '-Wno-deprecated', '-Wno-unused',
                        '-Wno-sign-compare', '-std=c++11', '-march=native', '-mtune=native', '-pipe']
        if debug:
            compile_args += ['-ggdb']

        if sys.platform == 'darwin':
            compile_args += ['-stdlib=libc++']

        link_args = []
        if debug:
            link_args += ['-ggdb']

        # Move static libraries to extra_objects (with path) to ensure static linking in posix systems
        extra_objects.extend(sum((['{}/lib{}.a'.format(lib_dir, l) for l in libs]
                                  for lib_dir, libs in static_libraries), []))

        # Add OpenMP support
        # Disable OpenMP on Mac see https://github.com/alejandrobll/py-sphviewer/issues/3
        if openmp and not sys.platform == 'darwin':
            compile_args.append('-fopenmp')
            link_args.append("-fopenmp")
            libraries.append('gomp')

    # Get the source files
    cmf_files = get_source_files()

    if swig:
        # Adding cmf.i when build_ext should perform the swig call
        cmf_files.append("cmf/cmf_core_src/cmf.i")
        swig_opts = ['-c++', '-Wextra', '-w512', '-w511', '-O', '-keyword', '-castmode', '-modern']

    else:
        # Else use what we have there
        cmf_files.append("cmf/cmf_core_src/cmf_wrap.cpp")
        swig_opts = []
    print('libraries:',' '.join(libraries))
    print('library_dirs:', ' '.join(library_dirs))
    print('include_dirs:', ' '.join(include_dirs))
    print('extra_objects:', ' '.join(extra_objects))
    cmf_core = Extension('cmf._cmf_core',
                         sources=cmf_files,
                         libraries=libraries,
                         library_dirs=library_dirs,
                         include_dirs=include_dirs,
                         extra_objects=extra_objects,
                         extra_compile_args=compile_args,
                         extra_link_args=link_args,
                         swig_opts=swig_opts
                         )
    return cmf_core


if __name__ == '__main__':
    updateversion()
    openmp = not pop_arg('noopenmp')
    swig = pop_arg('swig')
    debug = not pop_arg('nodebug')
    ext = [make_cmf_core()]
    description = 'Catchment Modelling Framework - A hydrological modelling toolkit'
    long_description = io.open('README.rst', encoding='utf-8').read()
    classifiers = [
        'Development Status :: 4 - Beta',
        'Intended Audience :: Science/Research',
        'License :: OSI Approved :: GNU General Public License v3 or later (GPLv3+)',
        'Programming Language :: C++',
        'Programming Language :: C',
        'Programming Language :: Python',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3.5',
        'Programming Language :: Python :: 3.6',
        'Topic :: Scientific/Engineering',
        'Topic :: Software Development :: Libraries :: Python Modules',
    ]

    setup(name='cmf',
          version=version,
          license='GPLv3+',
          ext_modules=ext,
          packages=['cmf', 'cmf.draw', 'cmf.geometry'],
          python_requires='>=3.5',
          keywords='hydrology catchment simulation toolbox',
          author='Philipp Kraft',
          author_email="philipp.kraft@umwelt.uni-giessen.de",
          url="https://www.uni-giessen.de/hydro/download",
          description=description,
          long_description=long_description,
          classifiers=classifiers,
          cmdclass=dict(build_py=build_py,
                        build_ext=CmfBuildExt),
          )

