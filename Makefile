.PHONY: help setup sync export eval train-stage1 train-stage2 maps core core-test backend-dev web-dev

help:
	@echo "IDR 26168 Makefile Commands:"
	@echo "  make setup          - Initialize python virtualenv and node packages"
	@echo "  make sync           - Run IO-VNBD sensor and reference synchronization"
	@echo "  make maps           - Build regional offline SQLite maps"
	@echo "  make export         - Export PyTorch models to ONNX"
	@echo "  make eval           - Run benchmark evaluation suite"
	@echo "  make core           - Compile C++ navigation engine"
	@echo "  make core-test      - Run C++ unit and replay tests"
	@echo "  make backend-dev    - Run backend Fastify service locally"
	@echo "  make web-dev        - Run Vite frontend dev server"

setup:
	python3 -m venv .venv
	@echo "Setup completed successfully."

sync:
	PYTHONPATH=ml python3 -m idr_ml.sync.sync_iovnbd

maps:
	python3 maps/scripts/build_india_maps.py

export:
	PYTHONPATH=ml python3 ml/idr_ml/export/export_onnx.py

eval:
	PYTHONPATH=ml python3 ml/idr_ml/eval/benchmark.py

train-stage1:
	PYTHONPATH=ml python3 ml/idr_ml/train/train_speed.py

train-stage2:
	PYTHONPATH=ml python3 ml/idr_ml/train/train_adapter.py

core:
	cmake -B build/core -S core -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/core

core-test: core
	ctest --test-dir build/core --output-on-failure

backend-dev:
	cd backend && pnpm dev

web-dev:
	cd web && pnpm dev
