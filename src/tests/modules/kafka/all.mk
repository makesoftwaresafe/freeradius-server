#
#  Test the "kafka" module.
#
#  Skipped unless KAFKA_TEST_SERVER is set in the environment.  CI provides it
#  via the Redpanda service declared in .github/workflows/ci.yml.
#
#  For local development:
#
#      docker run --rm -d --name fr-kafka -p 9092:9092 \
#          docker.redpanda.com/redpandadata/redpanda:latest
#      KAFKA_TEST_SERVER=127.0.0.1 make test.modules.kafka
#      docker stop fr-kafka
#
#  librdkafka auto-creates topics on first produce, so there is no setup
#  script (unlike ldap / 389ds / mysql).
#
kafka_require_test_server := 1
