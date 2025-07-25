# Auto generated for debiansid
# from scripts/docker/m4/docker.deb.m4
#
# Rebuild this file with `make docker.debiansid.regen`
#
ARG from=debian:sid
FROM ${from} AS build

ARG DEBIAN_FRONTEND=noninteractive

#
#  Install build tools
#
RUN apt-get update
RUN apt-get install -y devscripts equivs git quilt gcc curl

#
#  Set up NetworkRADIUS extras repository
#
RUN install -d -o root -g root -m 0755 /etc/apt/keyrings \
 && curl -o /etc/apt/keyrings/packages.networkradius.com.asc "https://packages.inkbridgenetworks.com/pgp/packages%40networkradius.com" \
 && echo "deb [signed-by=/etc/apt/keyrings/packages.networkradius.com.asc] http://packages.networkradius.com/extras/debian/sid sid main" > /etc/apt/sources.list.d/networkradius-extras.list \
 && apt-get update

#
#  Create build directory
#
RUN mkdir -p /usr/local/src/repositories/freeradius-server
WORKDIR /usr/local/src/repositories/freeradius-server/

#
#  Copy the FreeRADIUS directory in
#
COPY . .

#
#  Clean up tree - we want to build from the latest commit, not from
#  any cruft left around on the local system
#
RUN git clean -fdxx \
 && git reset --hard HEAD

#
#  Install build dependencies
#
RUN if [ -e ./debian/control.in ]; then \
        debian/rules debian/control; \
    fi; \
    echo 'y' | mk-build-deps -irt'apt-get -yV' debian/control

#
#  Build the server
#
RUN make -j$(nproc) deb

#
#  Clean environment and run the server
#
FROM ${from}
ARG DEBIAN_FRONTEND=noninteractive

COPY --from=build /usr/local/src/repositories/*.deb /tmp/

#
#  We need curl to get the signing key
#
RUN apt-get update \
 && apt-get install -y curl \
 && apt-get clean \
 && rm -r /var/lib/apt/lists/*

#
#  Set up NetworkRADIUS extras repository
#
RUN install -d -o root -g root -m 0755 /etc/apt/keyrings \
 && curl -o /etc/apt/keyrings/packages.networkradius.com.asc "https://packages.inkbridgenetworks.com/pgp/packages%40networkradius.com" \
 && echo "deb [signed-by=/etc/apt/keyrings/packages.networkradius.com.asc] http://packages.networkradius.com/extras/debian/sid sid main" > /etc/apt/sources.list.d/networkradius-extras.list

ARG freerad_uid=101
ARG freerad_gid=101

RUN groupadd -g ${freerad_gid} -r freerad \
 && useradd -u ${freerad_uid} -g freerad -r -M -d /etc/freeradius -s /usr/sbin/nologin freerad \
 && apt-get update \
 && apt-get install -y /tmp/*.deb \
 && apt-get clean \
 && rm -r /var/lib/apt/lists/* /tmp/*.deb \
    \
 && ln -s /etc/freeradius /etc/raddb

WORKDIR /
COPY scripts/docker/etc/docker-entrypoint.sh.deb docker-entrypoint.sh
RUN chmod +x docker-entrypoint.sh

EXPOSE 1812/udp 1813/udp
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["freeradius"]
