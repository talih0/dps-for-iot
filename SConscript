import os
import string
Import(['env', 'ext_libs'])

platform = env['PLATFORM']

env['UV_LIBS'].append(ext_libs)

# Additional warning for the lib object files

# Core libraries
libenv = env.Clone()

# Additional warnings for the core object files
if platform == 'win32':
    libenv.Append(LIBS = env['UV_LIBS'])
elif platform == 'posix':
    libenv.Append(CCFLAGS = ['-Wall', '-Wno-format-extra-args'])

libenv.Install('#/build/dist/inc/dps', libenv.Glob('#/inc/dps/*.h'))
libenv.Install('#/build/dist/inc/dps/private', libenv.Glob('#/inc/dps/private/*.h'))

srcs = ['src/bitvec.c',
        'src/cbor.c',
        'src/cose.c',
        'src/coap.c',
        'src/dps.c',
        'src/pub.c',
        'src/sub.c',
        'src/ack.c',
        'src/dbg.c',
        'src/err.c',
        'src/event.c',
        'src/history.c',
        'src/keystore.c',
        'src/synchronous.c',
        'src/uuid.c',
        'src/linkmon.c',
        'src/netmcast.c',
        'src/network.c',
        'src/registration.c',
        'src/resolver.c',
        'src/topics.c',
        'src/uv_extra.c',
        'src/sha2.c',
        'src/ccm.c',
        'src/ec.c',
        'src/hkdf.c',
        'src/keywrap.c',
        'src/mbedtls.c']

if env['transport'] == 'udp':
    srcs.append('src/udp/network.c')
elif env['transport'] == 'dtls':
    srcs.append('src/dtls/network.c')
else:
    srcs.append('src/tcp/network.c')

Depends(srcs, ext_libs)

objs = libenv.Object(srcs)

lib = libenv.Library('lib/dps', objs)
libenv.Install('#/build/dist/lib', lib)

shobjs = libenv.SharedObject(srcs)
if platform == 'win32':
    shlib = libenv.SharedLibrary('lib/dps_shared', shobjs + ['dps_shared.def'])
else:
    shlib = libenv.SharedLibrary('lib/dps_shared', shobjs)
libenv.Install('#/build/dist/lib', shlib)

ns3srcs = ['src/bitvec.c',
           'src/cbor.c',
           'src/ccm.c',
           'src/coap.c',
           'src/cose.c',
           'src/dps.c',
           'src/pub.c',
           'src/sha2.c',
           'src/sub.c',
           'src/ack.c',
           'src/err.c',
           'src/history.c',
           'src/uuid.c',
           'src/topics.c']

if platform == 'posix':
    ns3shobjs = libenv.SharedObject(ns3srcs)
    ns3shlib = libenv.SharedLibrary('lib/dps_ns3', ns3shobjs, LIBS = ext_libs)
    libenv.Install('#/build/dist/lib', ns3shlib)

if env['python']:
    # Using SWIG to build the python wrapper
    pyenv = libenv.Clone()
    pyenv.Append(LIBPATH = env['PY_LIBPATH'])
    pyenv.Append(CPPPATH = env['PY_CPPPATH'])
    pyenv.Append(LIBS = [lib, env['UV_LIBS']])
    # Python has platform specific naming conventions
    pyenv['SHLIBPREFIX'] = '_'
    if platform == 'win32':
        pyenv['SHLIBSUFFIX'] = '.pyd'

    pyenv.Append(SWIGFLAGS = ['-python', '-Wextra', '-Werror', '-v', '-O'], SWIGPATH = '#/inc')
    if platform == 'posix':
        pyenv.Append(CPPFLAGS = ['-Wno-strict-aliasing'])
    # Build python module library
    pylib = pyenv.SharedLibrary('./py/dps', shobjs + ['swig/dps_python.i'])
    pyenv.Install('#/build/dist/py', pylib)
    pyenv.InstallAs('#/build/dist/py/dps.py', './swig/dps.py')

if env['nodejs']:
    # Use SWIG to build the node.js wrapper
    #
    # Some shenanigans are needed here to get SWIG to correctly handle
    # the DPS_Key union: SWIGFLAGS must not include "-c++" and we then
    # need to tell the env to compile the genereated .c file as a cpp
    # file.
    #
    if platform == 'posix':
        nodeenv = libenv.Clone();
        nodeenv.Append(SWIGFLAGS = ['-javascript', '-node', '-DV8_VERSION=0x04059937', '-Wextra', '-Werror', '-v', '-O'], SWIGPATH = '#/inc')
        # There may be a bug with the SWIG builder - add -O to CPPFLAGS to get it passed on to the compiler
        nodeenv.Append(CPPFLAGS = ['-DBUILDING_NODE_EXTENSION', '-std=c++11', '-O'])
        nodeenv['CC'] = '$CXX'
        if env['target'] == 'yocto':
            nodeenv.Append(CPPPATH = [os.getenv('SYSROOT') + '/usr/include/node'])
        else:
            nodeenv.Append(CPPPATH = ['/usr/include/node'])
        nodeenv.Append(LIBS = [lib, env['UV_LIBS']])
        nodedps = nodeenv.SharedLibrary('lib/nodedps', shobjs + ['swig/dps_node.i'])
        nodeenv.InstallAs('#/build/dist/js/dps.node', nodedps)

# Unit tests
testenv = env.Clone()
if testenv['PLATFORM'] == 'win32':
    testenv.Append(CPPDEFINES = ['_CRT_SECURE_NO_WARNINGS'])
testenv.Append(CPPPATH = ['src'])
testenv.Append(LIBS = [lib, env['UV_LIBS']])

testsrcs = ['test/hist_unit.c',
            'test/make_mesh.c',
            'test/mesh_stress.c',
            'test/countvec.c',
            'test/rle_compression.c',
            'test/topic_match.c',
            'test/pubsub.c',
            'test/packtest.c',
            'test/cbortest.c',
            'test/cosetest.c',
            'test/version.c',
            'test/keystoretest.c']

Depends(testsrcs, ext_libs)

testprogs = []
for test in testsrcs:
    testprogs.append(testenv.Program(test))

testenv.Install('#/build/test/bin', testprogs)


# Examples
exampleenv = env.Clone()
if exampleenv['PLATFORM'] == 'win32':
    exampleenv.Append(CPPDEFINES = ['_CRT_SECURE_NO_WARNINGS'])
exampleenv.Append(LIBS = [lib, env['UV_LIBS']])

examplesrcs = ['examples/pub_many.c',
               'examples/publisher.c',
               'examples/reg_pubs.c',
               'examples/reg_subs.c',
               'examples/subscriber.c',
               'examples/registry.c']

Depends(examplesrcs, ext_libs)

exampleprogs = []
for example in examplesrcs:
    exampleprogs.append(exampleenv.Program([example, 'examples/keys.c']))

exampleenv.Install('#/build/dist/bin', exampleprogs)

# Documentation
try:
    libenv.Doxygen('doc/Doxyfile')
except:
    # Doxygen may not be installed
    pass
