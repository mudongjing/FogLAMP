# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

import os
import subprocess
from pathlib import Path
from aiohttp import web
from foglamp.services.core.snapshot import SnapshotPluginBuilder
from foglamp.common.common import _FOGLAMP_ROOT, _FOGLAMP_DATA

__author__ = "Ashish Jabble"
__copyright__ = "Copyright (c) 2017 OSIsoft, LLC"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"


_help = """
    -------------------------------------------------------------------------------
    | GET POST        | /foglamp/plugins/snapshot                                 |
    | PUT DELETE      | /foglamp/plugins/snapshot/{id}                            |
    -------------------------------------------------------------------------------
"""


async def get_snapshot(request):
    """ get list of available snapshots

    :Example:
        curl -X GET http://localhost:8081/foglamp/plugins/snapshot
    """
    # Get snapshot directory path
    snapshot_dir = _get_snapshot_dir()
    valid_extension = '.tar.gz'
    found_files = []
    for root, dirs, files in os.walk(snapshot_dir):
        found_files = [f for f in files if f.endswith(valid_extension)]

    return web.json_response({"snapshots": found_files})


async def post_snapshot(request):
    """ Create a snapshot  by name

    :Example:
        curl -X POST http://localhost:8081/foglamp/plugins/snapshot
    """
    snapshot_dir = _get_snapshot_dir()
    try:
        snapshot_name = await SnapshotPluginBuilder(snapshot_dir).build()
    except Exception as ex:
        raise web.HTTPInternalServerError(reason='Support could not be created. {}'.format(str(ex)))

    return web.json_response({"snapshot created": snapshot_name})


async def put_snapshot(request):
    """extract a snapshot

    :Example:
        curl -X PUT http://localhost:8081/foglamp/plugins/snapshot/snapshot-180311-18-03-36.tar.gz -H "Accept-Encoding: gzip" --write-out "size_download=%{size_download}\n" --compressed
    """
    snapshot_name = request.match_info.get('id', None)

    if not str(snapshot_name).endswith('.tar.gz'):
        return web.HTTPBadRequest(reason="Snapshot file extension is invalid")

    if not os.path.isdir(_get_snapshot_dir()):
        raise web.HTTPNotFound(reason="Snapshot directory does not exist")

    snapshot_dir = _get_snapshot_dir()
    for root, dirs, files in os.walk(snapshot_dir):
        if str(snapshot_name) not in files:
            raise web.HTTPNotFound(reason='{} not found'.format(snapshot_name))

    try:
        p = "{}/{}",format(snapshot_dir, snapshot_name)
        retval = await SnapshotPluginBuilder(snapshot_dir).extract_files(p)
    except Exception as ex:
        raise web.HTTPInternalServerError(reason='Snapshot {} could not be restored. {}'.format(snapshot_name, str(ex)))
    else:
        return web.json_response({"snapshot restored successfully": snapshot_name})


async def delete_snapshot(request):
    """delete a snapshot

    :Example:
        curl -X DELETE http://localhost:8081/foglamp/plugins/snapshot/snapshot-180311-18-03-36.tar.gz
    """
    snapshot_name = request.match_info.get('id', None)

    if not str(snapshot_name).endswith('.tar.gz'):
        return web.HTTPBadRequest(reason="Snapshot file extension is invalid")

    if not os.path.isdir(_get_snapshot_dir()):
        raise web.HTTPNotFound(reason="Snapshot directory does not exist")

    snapshot_dir = _get_snapshot_dir()
    for root, dirs, files in os.walk(_get_snapshot_dir()):
        if str(snapshot_name) not in files:
            raise web.HTTPNotFound(reason='{} not found'.format(snapshot_name))

    try:
        p = "{}/{}".format(snapshot_dir, snapshot_name)
        os.remove(p)
    except Exception as ex:
        raise web.HTTPInternalServerError(reason='Snapshot {} could not be deleted. {}'.format(snapshot_name, str(ex)))
    else:
        return web.json_response({"snapshot deleted successfully": snapshot_name})


def _get_snapshot_dir():
    if _FOGLAMP_DATA:
        snapshot_dir = os.path.expanduser(_FOGLAMP_DATA + '/tmp/snapshot')
    else:
        snapshot_dir = os.path.expanduser(_FOGLAMP_ROOT + '/data/tmp/snapshot')

    return snapshot_dir
