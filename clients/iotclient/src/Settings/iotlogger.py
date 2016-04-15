import logging
import os
import sys

if sys.platform in ("linux", "linux2", "darwin"):
    logging_location = '/var/log/iotdaemon/iot.log'
elif sys.platform == "win32":
    logging_location = os.path.relpath("iot.log")

logging_format = '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
logger = logging.getLogger("IOTLog")


def set_logging_location(new_location):
    global logging_location
    logging_location = new_location


def init_logging():
    global logger
    try:
        logger = logging.getLogger("IOTLog")
        logger.setLevel(logging.DEBUG)
        formatter = logging.Formatter(logging_format)

        logging_path = os.path.dirname(logging_location)
        if not os.path.exists(logging_path):
            os.makedirs(logging_path)
        log_handler = logging.FileHandler(logging_location, mode='a+')
        log_handler.setFormatter(formatter)
        logger.addHandler(log_handler)
    except Exception as ex:
        print >> sys.stderr, ex.message
        sys.exit(1)
