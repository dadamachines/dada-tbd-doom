FROM ubuntu:24.04 as platformio_builder

RUN apt-get update && apt-get install -y --no-install-recommends curl python3-pip python3-venv git
RUN curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py  \
    && python3 get-platformio.py
