FROM ubuntu:xenial

RUN apt-get update

RUN apt-get install -y libgmp3-dev libntl-dev libssl-dev python-dev build-essential cmake
RUN apt-get install -y python-cherrypy3 python-requests python-cffi python-twisted
RUN apt-get install -y letsencrypt gettext-base

ADD dp5 /dp5

RUN cd /dp5 && mkdir build && cd build && cmake .. && make
RUN cd /dp5 && cd build && python setup.py install

ADD run_server.sh /run_server.sh
ADD server.cfg /server.cfg

EXPOSE 443 8443
ENV DP5_ISREG="false" DP5_ISLOOKUP="true"

CMD bash /run_server.sh
