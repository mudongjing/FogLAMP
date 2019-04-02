# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

""" Provides utility functions to take snapshot of Foglamp"""

import datetime
import os
from os.path import basename
import glob
import json
import tarfile
import fnmatch
import time

from foglamp.common import logger
from foglamp.common.common import _FOGLAMP_ROOT


__author__ = "Amarendra K Sinha"
__copyright__ = "Copyright (c) 2017 OSIsoft, LLC"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"

_LOGGER = logger.setup(__name__)
_NO_OF_FILES_TO_RETAIN = 3


class SnapshotPluginBuilder:

    _out_file_path = None
    _interim_file_path = None

    def __init__(self, snapshot_plugin_dir):
        try:
            if not os.path.exists(snapshot_plugin_dir):
                os.makedirs(snapshot_plugin_dir)
            else:
                self.check_and_delete_bundles(snapshot_plugin_dir)

            self._out_file_path = snapshot_plugin_dir
            self._interim_file_path = snapshot_plugin_dir
        except (OSError, Exception) as ex:
            _LOGGER.error("Error in initializing SnapshotPluginBuilder class: %s ", str(ex))
            raise RuntimeError(str(ex))

    async def build(self):
        try:
            snapshot_id = str(int(time.time()))
            tar_file_name = self._out_file_path+"/"+"snapshot-plugin-{}.tar.gz".format(snapshot_id)
            pyz = tarfile.open(tar_file_name, "w:gz")
            foglamp_dir = _FOGLAMP_ROOT.split("/python")[0]
            try:
                pyz.add("{}/python/foglamp/plugins".format(foglamp_dir), recursive=True)
                pyz.add("{}/C/plugins".format(foglamp_dir), recursive=True)
            finally:
                pyz.close()
        except Exception as ex:
            _LOGGER.error("Error in creating Snapshot .tar.gz file: %s ", str(ex))
            raise RuntimeError(str(ex))

        self.check_and_delete_temp_files(self._interim_file_path)
        _LOGGER.info("Snapshot bundle %s successfully created.", tar_file_name)
        return tar_file_name

    def check_and_delete_bundles(self, snapshot_plugin_dir):
        files = glob.glob(snapshot_plugin_dir + "/" + "snapshot-plugin*.tar.gz")
        files.sort(key=os.path.getmtime)
        if len(files) >= _NO_OF_FILES_TO_RETAIN:
            for f in files[:-2]:
                if os.path.isfile(f):
                    os.remove(os.path.join(snapshot_plugin_dir, f))

    def check_and_delete_temp_files(self, snapshot_plugin_dir):
        # Delete all non *.tar.gz files
        for f in os.listdir(snapshot_plugin_dir):
            if not fnmatch.fnmatch(f, 'snapshot-plugin*.tar.gz'):
                os.remove(os.path.join(snapshot_plugin_dir, f))

    def write_to_tar(self, pyz, temp_file, data):
        with open(temp_file, 'w') as outfile:
            json.dump(data, outfile, indent=4)
        pyz.add(temp_file, arcname=basename(temp_file))

    def extract_files(self, pyz):
        try:
            with tarfile.open(pyz, "r:gz") as tar:
                # Since we are storing full path of the files, we need to specify "/" as the path to restore
                tar.extractall(path="/", members=tar.getmembers())
        except Exception as ex:
            raise RuntimeError("Extraction error for snapshot {}. {}".format(pyz, str(ex)))
        else:
            return True
