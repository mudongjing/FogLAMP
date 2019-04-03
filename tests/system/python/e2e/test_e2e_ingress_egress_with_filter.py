# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

""" Test end to end flow with:
        HTTP south plugin
        PI Server (C) plugin
        Scale filter plugin (on both Ingress & Egress with different scale & offset value)
"""

import os
import subprocess
import http.client
import json
import time
import pytest
import utils


__author__ = "Ashish Jabble"
__copyright__ = "Copyright (c) 2019 Dianomic Systems"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"


SOUTH_PLUGIN = "http_south"
SVC_NAME = "Http #1"
ASSET_PREFIX = "http-"
ASSET_NAME = "Http"

TASK_NAME = "North 2 PI"

FILTER_PLUGIN = "scale"
INGRESS_FILTER_NAME = "F#1 Scale"
EGRESS_FILTER_NAME = "F2 Scale"
OFFSET = 13

TEMPLATE_NAME = "template.json"
READ_KEY = "sensor"
SENSOR_VALUE = 20

# default scale factor
MULTIPLIER = 100.0
OUTPUT_1 = SENSOR_VALUE * MULTIPLIER
OUTPUT_2 = (OUTPUT_1 * MULTIPLIER) + OFFSET


class TestE2eIngressEgressWithFilter:

    def get_ping_status(self, foglamp_url):
        _connection = http.client.HTTPConnection(foglamp_url)
        _connection.request("GET", '/foglamp/ping')
        r = _connection.getresponse()
        assert 200 == r.status
        r = r.read().decode()
        jdoc = json.loads(r)
        return jdoc

    def get_statistics_map(self, foglamp_url):
        _connection = http.client.HTTPConnection(foglamp_url)
        _connection.request("GET", '/foglamp/statistics')
        r = _connection.getresponse()
        assert 200 == r.status
        r = r.read().decode()
        jdoc = json.loads(r)
        return utils.serialize_stats_map(jdoc)

    @pytest.fixture
    def start_south_north(self, reset_and_start_foglamp, add_south, remove_data_file, remove_directories,
                          south_branch, foglamp_url, add_filter, filter_branch, filter_name,
                          start_north_pi_server_c, pi_host, pi_port, pi_token):
        """ This fixture clone a south repo and starts both south and PI north C instance
            reset_and_start_foglamp: Fixture that resets and starts foglamp, no explicit invocation, called at start
            add_south: Fixture that adds a south service with given configuration with enabled mode
            remove_data_file: Fixture that remove data file created during the test
            remove_directories: Fixture that remove directories created during the test
            add_filter: Fixture that add a filter with given configuration with enabled mode
        """
        # Define the template file for fogbench
        fogbench_template_path = os.path.join(
            os.path.expandvars('${FOGLAMP_ROOT}'), 'data/{}'.format(TEMPLATE_NAME))
        with open(fogbench_template_path, "w") as f:
            f.write(
                '[{"name": "%s", "sensor_values": '
                '[{"name": "%s", "type": "number", "min": %d, "max": %d, "precision": 0}]}]' % (
                    ASSET_NAME, READ_KEY, SENSOR_VALUE, SENSOR_VALUE))

        # add south & INGRESS_FILTER_NAME
        filter_cfg = {"enable": "true"}
        add_south(SOUTH_PLUGIN, south_branch, foglamp_url, service_name=SVC_NAME)
        add_filter(FILTER_PLUGIN, filter_branch, INGRESS_FILTER_NAME, filter_cfg, foglamp_url, SVC_NAME)

        # add north & EGRESS_FILTER_NAME
        filter_cfg = {"offset": str(OFFSET), "enable": "true"}
        start_north_pi_server_c(foglamp_url, pi_host, pi_port, pi_token, taskname=TASK_NAME)
        add_filter(FILTER_PLUGIN, filter_branch, EGRESS_FILTER_NAME, filter_cfg, foglamp_url, TASK_NAME)

        yield self.start_south_north

        remove_data_file(fogbench_template_path)
        remove_directories("/tmp/foglamp-south-{}".format(ASSET_NAME.lower()))
        remove_directories("/tmp/foglamp-filter-{}".format(FILTER_PLUGIN))

    def test_end_to_end(self, start_south_north, read_data_from_pi, foglamp_url, pi_host, pi_admin,
                        pi_passwd, pi_db, wait_time, retries, skip_verify_north_interface,
                        disable_schedule, enable_schedule):
        # ingest first reading via fogbench
        subprocess.run(["cd $FOGLAMP_ROOT/extras/python; python3 -m fogbench -t ../../data/{} -p http; cd -".format(
            TEMPLATE_NAME)], shell=True, check=True)
        # Let the readings to be ingressed & extra wait time needed for sent
        time.sleep(wait_time * 2)

        # verify ping and statistics
        self._verify_ping_and_statistics(1, foglamp_url)

        # verify ingest with first reading
        conn = http.client.HTTPConnection(foglamp_url)
        self._verify_ingest(conn, 1, OUTPUT_1)

        # verify egress with first reading
        if not skip_verify_north_interface:
            self._verify_egress(read_data_from_pi, pi_host, pi_admin, pi_passwd, pi_db, wait_time,
                                retries, OUTPUT_1)

        # Disable TASK_NAME schedule
        disable_schedule(foglamp_url, TASK_NAME)

        # ingest second reading via fogbench
        subprocess.run(["cd $FOGLAMP_ROOT/extras/python; python3 -m fogbench -t ../../data/{} -p http; cd -"
                       .format(TEMPLATE_NAME)], shell=True, check=True)

        # Enable TASK_NAME schedule to pick filter config
        enable_schedule(foglamp_url, TASK_NAME)

        # extra wait time needed for sent
        time.sleep(wait_time * 2)

        # verify ping and statistics
        self._verify_ping_and_statistics(2, foglamp_url)

        # verify ingest with second reading
        self._verify_ingest(conn, 2, OUTPUT_1)

        # verify egress with second reading
        if not skip_verify_north_interface:
            self._verify_egress(read_data_from_pi, pi_host, pi_admin, pi_passwd, pi_db, wait_time,
                                retries, OUTPUT_2)

    def _verify_ping_and_statistics(self, count, foglamp_url):
        ping_response = self.get_ping_status(foglamp_url)
        assert count == ping_response["dataRead"]
        assert count == ping_response["dataSent"]

        actual_stats_map = self.get_statistics_map(foglamp_url)
        key = "{}{}".format(ASSET_PREFIX.upper(), ASSET_NAME.upper())
        assert count == actual_stats_map[key]
        assert count == actual_stats_map[TASK_NAME]
        assert count == actual_stats_map['READINGS']
        assert count == actual_stats_map['Readings Sent']

    def _verify_ingest(self, conn, read_count, value):
        conn.request("GET", '/foglamp/asset')
        r = conn.getresponse()
        assert 200 == r.status
        r = r.read().decode()
        jdoc = json.loads(r)
        assert len(jdoc), "No data found"
        assert "{}{}".format(ASSET_PREFIX, ASSET_NAME) == jdoc[0]["assetCode"]
        assert read_count == jdoc[0]["count"]

        conn.request("GET", '/foglamp/asset/{}{}'.format(ASSET_PREFIX, ASSET_NAME))
        r = conn.getresponse()
        assert 200 == r.status
        r = r.read().decode()
        jdoc = json.loads(r)
        assert len(jdoc), "No data found"
        # verify INGRESS scale filter is applied and O/P with (readings * 100) on BOTH reading case
        assert value == jdoc[0]["reading"][READ_KEY]
        if read_count == 2:
            assert value == jdoc[1]["reading"][READ_KEY]

    def _verify_egress(self, read_data_from_pi, pi_host, pi_admin, pi_passwd, pi_db, wait_time, retries, value):
        retry_count = 0
        data_from_pi = None
        while (data_from_pi is None or data_from_pi == []) and retry_count < retries:
            name = "{}{}".format(ASSET_PREFIX, ASSET_NAME)
            data_from_pi = read_data_from_pi(pi_host, pi_admin, pi_passwd, pi_db, name, {READ_KEY})
            retry_count += 1
            time.sleep(wait_time * 2)

        if data_from_pi is None or retry_count == retries:
            assert False, "Failed to read data from PI"

        assert len(data_from_pi), "No data found from pi"

        # verify OUTPUT_1 (readings * 100) on egress side with INGRESS scale filter
        # verify OUTPUT_2 (readings * 100 + OFFSET) on egress side with EGRESS scale filter
        assert READ_KEY in data_from_pi
        assert isinstance(data_from_pi[READ_KEY], list)
        assert value in data_from_pi[READ_KEY]
