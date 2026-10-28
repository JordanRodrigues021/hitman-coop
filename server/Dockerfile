# HITMAN Co-op relay server, created by Jordan Rodrigues.
#   docker build -t hitmancoop-relay server/
#   docker run -d --restart=always -p 47100:47100/udp hitmancoop-relay
FROM python:3.12-alpine
COPY relay.py /app/relay.py
USER nobody
EXPOSE 47100/udp
ENTRYPOINT ["python3", "-I", "/app/relay.py", "--port", "47100"]
