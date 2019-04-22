import hifi_utils
import hifi_android
import hashlib
import os
import platform
import re
import shutil
import tempfile
import json
import xml.etree.ElementTree as ET
import functools

print = functools.partial(print, flush=True)

# Encapsulates the vcpkg system 
class VcpkgRepo:
    CMAKE_TEMPLATE = """
get_filename_component(CMAKE_TOOLCHAIN_FILE "{}" ABSOLUTE CACHE)
get_filename_component(CMAKE_TOOLCHAIN_FILE_UNCACHED "{}" ABSOLUTE)
set(VCPKG_INSTALL_ROOT "{}")
set(VCPKG_TOOLS_DIR "{}")
"""

    CMAKE_TEMPLATE_NON_ANDROID = """
# If the cached cmake toolchain path is different from the computed one, exit
if(NOT (CMAKE_TOOLCHAIN_FILE_UNCACHED STREQUAL CMAKE_TOOLCHAIN_FILE))
    message(FATAL_ERROR "CMAKE_TOOLCHAIN_FILE has changed, please wipe the build directory and rerun cmake")
endif()
"""

    def __init__(self, args):
        self.args = args
        # our custom ports, relative to the script location
        self.sourcePortsPath = args.ports_path
        self.id = hifi_utils.hashFolder(self.sourcePortsPath)[:8]
        self.configFilePath = os.path.join(args.build_root, 'vcpkg.cmake')

        # OS dependent information
        system = platform.system()

        if self.args.vcpkg_root is not None:
            self.path = args.vcpkg_root
        else:
            if 'Darwin' == system:
                defaultBasePath = os.path.expanduser('~/hifi/vcpkg')
            else:
                defaultBasePath = os.path.join(tempfile.gettempdir(), 'hifi', 'vcpkg')
            self.basePath = os.getenv('HIFI_VCPKG_BASE', defaultBasePath)
            if self.basePath == defaultBasePath:
                print("Warning: Environment variable HIFI_VCPKG_BASE not set, using {}".format(defaultBasePath))
            if self.args.android:
                self.basePath = os.path.join(self.basePath, 'android')
            if (not os.path.isdir(self.basePath)):
                os.makedirs(self.basePath)
            self.path = os.path.join(self.basePath, self.id)

        print("Using vcpkg path {}".format(self.path))
        lockDir, lockName = os.path.split(self.path)
        lockName += '.lock'
        if not os.path.isdir(lockDir):
            os.makedirs(lockDir)

        self.lockFile = os.path.join(lockDir, lockName)
        self.tagFile = os.path.join(self.path, '.id')
        # A format version attached to the tag file... increment when you want to force the build systems to rebuild 
        # without the contents of the ports changing
        self.version = 1
        self.tagContents = "{}_{}".format(self.id, self.version)

        if 'Windows' == system:
            self.exe = os.path.join(self.path, 'vcpkg.exe')
            self.vcpkgUrl = 'https://hifi-public.s3.amazonaws.com/dependencies/vcpkg/vcpkg-win32.tar.gz?versionId=YZYkDejDRk7L_hrK_WVFthWvisAhbDzZ'
            self.vcpkgHash = '3e0ff829a74956491d57666109b3e6b5ce4ed0735c24093884317102387b2cb1b2cd1ff38af9ed9173501f6e32ffa05cc6fe6d470b77a71ca1ffc3e0aa46ab9e'
            self.hostTriplet = 'x64-windows'
        elif 'Darwin' == system:
            self.exe = os.path.join(self.path, 'vcpkg')
            self.vcpkgUrl = 'https://hifi-public.s3.amazonaws.com/dependencies/vcpkg/vcpkg-osx.tar.gz?versionId=_fhqSxjfrtDJBvEsQ8L_ODcdUjlpX9cc'
            self.vcpkgHash = '519d666d02ef22b87c793f016ca412e70f92e1d55953c8f9bd4ee40f6d9f78c1df01a6ee293907718f3bbf24075cc35492fb216326dfc50712a95858e9cbcb4d'
            self.hostTriplet = 'x64-osx'
        else:
            self.exe = os.path.join(self.path, 'vcpkg')
            self.vcpkgUrl = 'https://hifi-public.s3.amazonaws.com/dependencies/vcpkg/vcpkg-linux.tar.gz?versionId=97Nazh24etEVKWz33XwgLY0bvxEfZgMU'
            self.vcpkgHash = '6a1ce47ef6621e699a4627e8821ad32528c82fce62a6939d35b205da2d299aaa405b5f392df4a9e5343dd6a296516e341105fbb2dd8b48864781d129d7fba10d'
            self.hostTriplet = 'x64-linux'

        if self.args.android:
            self.triplet = 'arm64-android'
            self.androidPackagePath = os.getenv('HIFI_ANDROID_PRECOMPILED', os.path.join(self.path, 'android'))
        else:
            self.triplet = self.hostTriplet

    def upToDate(self):
        # Prevent doing a clean if we've explcitly set a directory for vcpkg
        if self.args.vcpkg_root is not None:
            return True

        if self.args.force_build:
            print("Force build, out of date")
            return False
        if not os.path.isfile(self.exe):
            print("Exe file {} not found, out of date".format(self.exe))
            return False
        if not os.path.isfile(self.tagFile):
            print("Tag file {} not found, out of date".format(self.tagFile))
            return False
        with open(self.tagFile, 'r') as f:
            storedTag = f.read()
        if storedTag != self.tagContents:
            print("Tag file {} contents don't match computed tag {}, out of date".format(self.tagFile, self.tagContents))
            return False
        return True

    def clean(self):
        print("Cleaning vcpkg installation at {}".format(self.path))
        if os.path.isdir(self.path):
            print("Removing {}".format(self.path))
            shutil.rmtree(self.path, ignore_errors=True)

    # Make sure the VCPKG prerequisites are all there.
    def bootstrap(self):
        if self.upToDate():
            return

        self.clean()

        downloadVcpkg = False
        if self.args.force_bootstrap:
            print("Forcing bootstrap")
            downloadVcpkg = True

        if not downloadVcpkg and not os.path.isfile(self.exe):
            print("Missing executable, boostrapping")
            downloadVcpkg = True
        
        # Make sure we have a vcpkg executable
        testFile = os.path.join(self.path, '.vcpkg-root')
        if not downloadVcpkg and not os.path.isfile(testFile):
            print("Missing {}, bootstrapping".format(testFile))
            downloadVcpkg = True

        if downloadVcpkg:
            print("Fetching vcpkg from {} to {}".format(self.vcpkgUrl, self.path))
            hifi_utils.downloadAndExtract(self.vcpkgUrl, self.path, self.vcpkgHash)

        print("Replacing port files")
        portsPath = os.path.join(self.path, 'ports')
        if (os.path.islink(portsPath)):
            os.unlink(portsPath)
        if (os.path.isdir(portsPath)):
            shutil.rmtree(portsPath, ignore_errors=True)
        shutil.copytree(self.sourcePortsPath, portsPath)

    def run(self, commands):
        actualCommands = [self.exe, '--vcpkg-root', self.path]
        actualCommands.extend(commands)
        print("Running command")
        print(actualCommands)
        hifi_utils.executeSubprocess(actualCommands, folder=self.path)

    def setupDependencies(self):
        # Special case for android, grab a bunch of binaries
        # FIXME remove special casing for android builds eventually
        if self.args.android:
            print("Installing Android binaries")
            self.setupAndroidDependencies()

        print("Installing host tools")
        self.run(['install', '--triplet', self.hostTriplet, 'hifi-host-tools'])

        # If not android, install the hifi-client-deps libraries
        if not self.args.android:
            print("Installing build dependencies")
            self.run(['install', '--triplet', self.triplet, 'hifi-client-deps'])
            
        # If not android, install our Qt build
        if not self.args.android:
            print("Installing Qt")
            self.installQt()

    def cleanBuilds(self):
        # Remove temporary build artifacts
        builddir = os.path.join(self.path, 'buildtrees')
        if os.path.isdir(builddir):
            print("Wiping build trees")
            shutil.rmtree(builddir, ignore_errors=True)

    def setupAndroidDependencies(self):
        # vcpkg prebuilt
        if not os.path.isdir(os.path.join(self.path, 'installed', 'arm64-android')):
            dest = os.path.join(self.path, 'installed')
            url = "https://hifi-public.s3.amazonaws.com/dependencies/vcpkg/vcpkg-arm64-android.tar.gz"
            # FIXME I don't know why the hash check frequently fails here.  If you examine the file later it has the right hash
            #hash = "832f82a4d090046bdec25d313e20f56ead45b54dd06eee3798c5c8cbdd64cce4067692b1c3f26a89afe6ff9917c10e4b601c118bea06d23f8adbfe5c0ec12bc3"
            #hifi_utils.downloadAndExtract(url, dest, hash)
            hifi_utils.downloadAndExtract(url, dest)

        print("Installing additional android archives")
        androidPackages = hifi_android.getPlatformPackages()
        for packageName in androidPackages:
            package = androidPackages[packageName]
            dest = os.path.join(self.androidPackagePath, packageName)
            if os.path.isdir(dest):
                continue
            url = hifi_android.getPackageUrl(package)
            zipFile = package['file'].endswith('.zip')
            print("Android archive {}".format(package['file']))
            hifi_utils.downloadAndExtract(url, dest, isZip=zipFile, hash=package['checksum'], hasher=hashlib.md5())

    def writeTag(self):
        print("Writing tag {} to {}".format(self.tagContents, self.tagFile))
        with open(self.tagFile, 'w') as f:
            f.write(self.tagContents)

    def writeConfig(self):
        print("Writing cmake config to {}".format(self.configFilePath))
        # Write out the configuration for use by CMake
        cmakeScript = os.path.join(self.path, 'scripts/buildsystems/vcpkg.cmake')
        installPath = os.path.join(self.path, 'installed', self.triplet)
        toolsPath = os.path.join(self.path, 'installed', self.hostTriplet, 'tools')
        cmakeTemplate = VcpkgRepo.CMAKE_TEMPLATE
        if not self.args.android:
            cmakeTemplate += VcpkgRepo.CMAKE_TEMPLATE_NON_ANDROID
        else:
            precompiled = os.path.realpath(self.androidPackagePath)
            qtCmakePrefix = os.path.realpath(os.path.join(precompiled, 'qt/lib/cmake'))
            cmakeTemplate += 'set(HIFI_ANDROID_PRECOMPILED "{}")\n'.format(precompiled)
            cmakeTemplate += 'set(QT_CMAKE_PREFIX_PATH "{}")\n'.format(qtCmakePrefix)

        cmakeConfig = cmakeTemplate.format(cmakeScript, cmakeScript, installPath, toolsPath).replace('\\', '/')
        with open(self.configFilePath, 'w') as f:
            f.write(cmakeConfig)

    def cleanOldBuilds(self):
        # FIXME because we have the base directory, and because a build will 
        # update the tag file on every run, we can scan the base dir for sub directories containing 
        # a tag file that is older than N days, and if found, delete the directory, recovering space
        print("Not implemented")


    def installQt(self):
        print("install Qt")
        if not os.path.isdir(os.path.join(self.path, 'installed', 'hifi-qt5')):
            dest = os.path.join(self.path, 'installed')
            if platform.system() == 'Windows':
                # url = "https://hifi-qa.s3.amazonaws.com/qt5/Windows/qt5-install.zip"
                url = "https://hifi-qa.s3.amazonaws.com/qt5/Windows/hifi-qt5.tar.gz"
            elif platform.system() == 'Darwin':
                url = "https://hifi-qa.s3.amazonaws.com/qt5/Mac/qt5-install.zip"
            elif platform.system() == 'Linux':
                url = "https://hifi-qa.s3.amazonaws.com/qt5/Ubuntu/qt5-install.zip"
            
            hifi_utils.downloadAndExtract(url, dest)
