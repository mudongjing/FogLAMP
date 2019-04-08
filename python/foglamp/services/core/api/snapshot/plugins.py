# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END


import os
from aiohttp import web
from foglamp.services.core.snapshot import SnapshotPluginBuilder
from foglamp.common.common import _FOGLAMP_ROOT, _FOGLAMP_DATA


__author__ = "Amarendra K Sinha"
__copyright__ = "Copyright (c) 2019 Diaonmic Systems"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"


_help = """
    -------------------------------------------------------------------------------
    | GET POST        | /foglamp/snapshot/plugins                                 |
    | PUT DELETE      | /foglamp/snapshot/plugins/{id}                    |
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
    find_id = lambda x: x.split("snapshot-plugin-")[1].split(".tar.gz")[0]

    for root, dirs, files in os.walk(snapshot_dir):
        found_files = [{"id": find_id(f), "name":f} for f in files if f.endswith(valid_extension)]

    return web.json_response({"snapshots": found_files})


async def post_snapshot(request):
    """ Create a snapshot  by name

    :Example:
        curl -X POST http://localhost:8081/foglamp/plugins/snapshot
    """
    snapshot_dir = _get_snapshot_dir()
    try:
        snapshot_id, snapshot_name = await SnapshotPluginBuilder(snapshot_dir).build()
    except Exception as ex:
        raise web.HTTPInternalServerError(reason='Snapshot could not be created. {}'.format(str(ex)))
    else:
        return web.json_response({"message": "snapshot id={}, file={} created successfully.".format(snapshot_id, snapshot_name)})


async def put_snapshot(request):
    """extract a snapshot

    :Example:
        curl -X PUT http://localhost:8081/foglamp/plugins/snapshot/1554204238
    """
    snapshot_id = request.match_info.get('id', None)
    snapshot_name = "snapshot-plugin-{}.tar.gz".format(snapshot_id)

    if not os.path.isdir(_get_snapshot_dir()):
        raise web.HTTPNotFound(reason="No snapshot found.")

    snapshot_dir = _get_snapshot_dir()
    for root, dirs, files in os.walk(snapshot_dir):
        if str(snapshot_name) not in files:
            raise web.HTTPNotFound(reason='{} not found'.format(snapshot_name))

    try:
        p = "{}/{}".format(snapshot_dir, snapshot_name)
        retval = SnapshotPluginBuilder(snapshot_dir).extract_files(p)
    except Exception as ex:
        raise web.HTTPInternalServerError(reason='Snapshot {} could not be restored. {}'.format(snapshot_name, str(ex)))
    else:
        return web.json_response({"message": "snapshot {} restored successfully.".format(snapshot_name)})


async def delete_snapshot(request):
    """delete a snapshot

    :Example:
        curl -X DELETE http://localhost:8081/foglamp/plugins/snapshot/1554204238
    """
    snapshot_id = request.match_info.get('id', None)
    snapshot_name = "snapshot-plugin-{}.tar.gz".format(snapshot_id)

    if not os.path.isdir(_get_snapshot_dir()):
        raise web.HTTPNotFound(reason="No snapshot found.")

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
        return web.json_response({"message": "snapshot {} deleted successfully.".format(snapshot_name)})


def _get_snapshot_dir():
    if _FOGLAMP_DATA:
        snapshot_dir = os.path.expanduser(_FOGLAMP_DATA + '/snapshots/plugins')
    else:
        snapshot_dir = os.path.expanduser(_FOGLAMP_ROOT + '/data/snapshots/plugins')
    return snapshot_dir
