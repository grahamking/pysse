from distutils.core import setup, Extension

pyssemod = Extension(
        'pysse',
        sources=['pysse.c', 'ext.c'],
        extra_compile_args=['-std=c99'])

setup(name='pysse',
      version='0.1',
      description='Python Server-Sent Events',
      ext_modules=[pyssemod])
