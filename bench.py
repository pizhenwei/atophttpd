#!/usr/bin/python
import requests
import time

url_status = {"favicon.ico": 200,\
              "index.html": 200,\
              "js/atop.js": 200,\
              "css/atop.css": 200,\
              "template?type=generic": 200,\
              "template?type=memory": 200,\
              "template?type=disk": 200,\
              "template?type=command_line": 200,\
              "showsamp": 200,\
              "showsamp?lables=ALL&timestamp=" + str(int(time.time())): 200,\
              "notexist": 404}

def bench():
    for num in range(0, 100):
        for url, status in url_status.items():
            request = 'http://127.0.0.1:2867/' + url
            result = requests.get(request)
            if result.status_code != status:
                sys.exit(request + " failed!")

def main():
    pingurl = 'http://127.0.0.1:2867/ping'
    ping = requests.get(pingurl)
    if ping.status_code != 200:
        sys.exit(pingurl + " failed!")

    bench()

if __name__ == "__main__":
    main()
