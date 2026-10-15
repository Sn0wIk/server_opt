FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends nginx ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY nginx/baseline.conf /etc/nginx/baseline.conf
RUN sed 's/worker_processes auto;/worker_processes 1;/' \
    /etc/nginx/baseline.conf > /etc/nginx/tuned.conf

USER 33:33
EXPOSE 8080
CMD ["nginx", "-c", "/etc/nginx/baseline.conf", "-g", "daemon off;"]
