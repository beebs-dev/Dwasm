
# syntax=docker/dockerfile:1

# Emscripten toolchain image tag. Consider pinning this in CI for reproducibility.
ARG EMSDK_TAG=latest

FROM emscripten/emsdk:${EMSDK_TAG} AS build

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		cmake \
		curl \
		git \
		make \
		ninja-build \
		pkg-config \
		libsdl2-dev \
	&& rm -rf /var/lib/apt/lists/*

# Build GL4ES for Emscripten (provides OpenGL->GLES translation used by this project).
# Source: https://ptitseb.github.io/gl4es/COMPILE.html
ARG GL4ES_REPO=https://github.com/ptitSeb/gl4es.git
ARG GL4ES_REF=master
RUN git clone --depth 1 --branch "${GL4ES_REF}" "${GL4ES_REPO}" /tmp/gl4es \
	&& mkdir -p /tmp/gl4es/build \
	&& cd /tmp/gl4es/build \
	&& emcmake cmake .. \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DNOX11=ON \
		-DNOEGL=ON \
		-DSTATICLIB=ON \
	&& cmake --build . -j"$(nproc)" \
	&& mkdir -p /opt/gl4es/include /opt/gl4es/lib \
	&& cp -a /tmp/gl4es/include/. /opt/gl4es/include/ \
	&& GL4ES_LIB="$(find /tmp/gl4es -name libGL.a -print -quit)" \
	&& test -n "${GL4ES_LIB}" \
	&& cp -a "${GL4ES_LIB}" /opt/gl4es/lib/libGL.a \
	&& rm -rf /tmp/gl4es

WORKDIR /src
COPY . .

# 1) Generate prboomx.wad (required by the WebAssembly build). This is done via the
#    native build because the rdatawad tool isn't built when cross-compiling.
RUN cmake -S . -B build_native \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_GL=OFF \
		-DWITH_IMAGE=OFF \
		-DWITH_MIXER=OFF \
		-DWITH_NET=OFF \
		-DWITH_PCRE=OFF \
		-DWITH_ZLIB=OFF \
		-DWITH_MAD=OFF \
		-DWITH_FLUIDSYNTH=OFF \
		-DWITH_DUMB=OFF \
		-DWITH_VORBISFILE=OFF \
		-DWITH_PORTMIDI=OFF \
		-DWITH_ALSA=OFF \
	&& cmake --build build_native --target prboomwad -j"$(nproc)" \
	&& install -Dm644 build_native/prboomx.wad wasm/fs/prboomx.wad

# 2) WebAssembly build (per README.md)
RUN mkdir -p build \
	&& cd build \
	&& emcmake cmake .. -DCMAKE_BUILD_TYPE=Release -DGL4ES_PATH=/opt/gl4es \
	&& cmake --build . -j"$(nproc)" \
	&& mkdir -p /out \
	&& cp -f index.html index.js index.data index.wasm /out/ \
	&& (test -f index.js.map && cp -f index.js.map /out/ || true) \
	&& (test -f index.wasm.map && cp -f index.wasm.map /out/ || true) \
	&& curl -fsSL "https://unpkg.com/livekit-client@2.16.1/dist/livekit-client.umd.js" -o /out/livekit-client.umd.js

# Export stage: use BuildKit output, e.g.
#   docker build --target export -o ./dist .
FROM scratch AS export
COPY --from=build /out/ /

# Runtime stage: serve the app via nginx.
FROM nginx:alpine AS runtime

COPY --from=build /out/ /usr/share/nginx/html/

# Ensure wasm uses the correct MIME type on nginx/alpine.
RUN grep -q "application/wasm" /etc/nginx/mime.types \
	|| sed -i 's/}/    application\/wasm  wasm;\n}/' /etc/nginx/mime.types

EXPOSE 80


