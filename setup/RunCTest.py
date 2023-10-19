from distutils.core import Command

class RunCTest(Command):
    description = "Run C test"
    user_options = []
    extra_compile_args = []
    extra_link_args = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        from distutils.ccompiler import new_compiler
        import distutils.sysconfig as sysconfig

        compiler = new_compiler()

        # Compile and link
        obj_files = compiler.compile(['src/rtpsynth.c', 'tests/test_synth.c'],
          extra_preargs=self.extra_compile_args + ['-Isrc',])
        compiler.link_executable(obj_files, 'build/test_synth', extra_postargs=self.extra_link_args)

        import subprocess
        subprocess.run(['./build/test_synth'])
