# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

""" Provides utility functions to take snapshot of plugins"""

import os
from os.path import basename
import glob
import json
import tarfile
import fnmatch
import time
import subprocess

from foglamp.common import logger
from foglamp.common.common import _FOGLAMP_ROOT


__author__ = "Amarendra K Sinha"
__copyright__ = "Copyright (c) 2019 Dianomic Systems"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"

_LOGGER = logger.setup(__name__)
_NO_OF_FILES_TO_RETAIN = 3
SNAPSHOT_PREFIX = "snapshot-plugin"

class SnapshotPluginBuilder:

    _out_file_path = None
    _interim_file_path = None

    def __init__(self, snapshot_plugin_dir):
        try:
            if not os.path.exists(snapshot_plugin_dir):
                os.makedirs(snapshot_plugin_dir)
            else:
                self.check_and_delete_plugins_tar_files(snapshot_plugin_dir)

            self._out_file_path = snapshot_plugin_dir
            self._interim_file_path = snapshot_plugin_dir
        except (OSError, Exception) as ex:
            _LOGGER.error("Error in initializing SnapshotPluginBuilder class: %s ", str(ex))
            raise RuntimeError(str(ex))

    async def build(self):
        def reset(tarinfo):
            tarinfo.uid = tarinfo.gid = 0
            tarinfo.uname = tarinfo.gname = "root"
            return tarinfo
        try:
            snapshot_id = str(int(time.time()))
            snapshot_filename = "{}-{}.tar.gz".format(SNAPSHOT_PREFIX, snapshot_id)
            tar_file_name = "{}/{}".format(self._out_file_path, snapshot_filename)
            pyz = tarfile.open(tar_file_name, "w:gz")
            try:
                pyz.add("{}/python/foglamp/plugins".format(_FOGLAMP_ROOT), recursive=True)
                # C plugins location is different with "make install" and "make"
                if _FOGLAMP_ROOT == '/usr/local/foglamp':
                    pyz.add("{}/plugins".format(_FOGLAMP_ROOT), recursive=True, filter=reset)
                else:
                    pyz.add("{}/C/plugins".format(_FOGLAMP_ROOT), recursive=True)
                    pyz.add("{}/plugins".format(_FOGLAMP_ROOT), recursive=True)
                    pyz.add("{}/cmake_build/C/plugins".format(_FOGLAMP_ROOT), recursive=True)
            finally:
                pyz.close()
        except Exception as ex:
            _LOGGER.error("Error in creating Snapshot .tar.gz file: %s ", str(ex))
            raise RuntimeError(str(ex))

        self.check_and_delete_temp_files(self._interim_file_path)
        _LOGGER.info("Snapshot %s successfully created.", tar_file_name)
        return snapshot_id, snapshot_filename

    def check_and_delete_plugins_tar_files(self, snapshot_plugin_dir):
        files = glob.glob("{}/{}*.tar.gz".format(snapshot_plugin_dir, SNAPSHOT_PREFIX))
        files.sort(key=os.path.getmtime)
        if len(files) >= _NO_OF_FILES_TO_RETAIN:
            for f in files[:-2]:
                if os.path.isfile(f):
                    os.remove(os.path.join(snapshot_plugin_dir, f))

    def check_and_delete_temp_files(self, snapshot_plugin_dir):
        # Delete all non *.tar.gz files
        for f in os.listdir(snapshot_plugin_dir):
            if not fnmatch.fnmatch(f, '{}*.tar.gz'.format(SNAPSHOT_PREFIX)):
                os.remove(os.path.join(snapshot_plugin_dir, f))

    def write_to_tar(self, pyz, temp_file, data):
        with open(temp_file, 'w') as outfile:
            json.dump(data, outfile, indent=4)
        pyz.add(temp_file, arcname=basename(temp_file))

    def extract_files(self, pyz):
        # Since we are storing full path of the files, we need to specify "/" as the path to restore
        if _FOGLAMP_ROOT == '/usr/local/foglamp':
            a = subprocess.Popen(["sudo", "/bin/tar", "-C", "/", "-xzf", pyz], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        else:
            a = subprocess.Popen(["/bin/tar", "-C", "/", "-xzf", pyz], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        outs, errs = a.communicate()
        retcode = a.returncode
        if retcode != 0:
            raise OSError(
                'Error {}: "{}". Error: {}'.format(retcode, "tar -xzf {} -C /".format(pyz), errs.decode('utf-8').replace('\n', '')))
        return True

        # TODO: Investigate why below does not work with sudo make install
        # try:
        #     with tarfile.open(pyz, "r:gz") as tar:
        #         # Since we are storing full path of the files, we need to specify "/" as the path to restore
        #         tar.extractall(path="/", members=tar.getmembers())
        # except Exception as ex:
        #     raise RuntimeError("Extraction error for snapshot {}. {}".format(pyz, str(ex)))
        # else:
        #     return True
