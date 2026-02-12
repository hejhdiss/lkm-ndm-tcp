# Makefile for NDM-TCP Linux Kernel Module
#
# Usage:
#   make              - Build the module
#   make install      - Install the module
#   make load         - Load the module into kernel
#   make unload       - Unload the module from kernel
#   make enable       - Set NDM-TCP as default congestion control
#   make disable      - Restore previous congestion control
#   make clean        - Clean build files
#   make test         - Run basic tests

# Module name
obj-m += ndm_tcp_lkm.o

# Compiler flags to avoid SSE issues
ccflags-y := -mno-sse -mno-sse2

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Default target
all:
	@echo "Building NDM-TCP kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "Build complete! Module: ndm_tcp_lkm.ko"

# Install module to system
install: all
	@echo "Installing NDM-TCP module..."
	sudo $(MAKE) -C $(KDIR) M=$(PWD) modules_install
	sudo depmod -a
	@echo "Module installed successfully"

# Load module into kernel
load:
	@echo "Loading NDM-TCP module..."
	-sudo rmmod ndm_tcp_lkm 2>/dev/null || true
	sudo insmod ndm_tcp_lkm.ko
	@echo "Module loaded. Check with: lsmod | grep ndm_tcp"
	@dmesg | tail -5

# Unload module from kernel
unload:
	@echo "Unloading NDM-TCP module..."
	sudo rmmod ndm_tcp_lkm
	@echo "Module unloaded"
	@dmesg | tail -3

# Enable NDM-TCP as default congestion control
enable: load
	@echo "Setting NDM-TCP as default congestion control..."
	@echo "Current setting: $$(sysctl net.ipv4.tcp_congestion_control)"
	sudo sysctl -w net.ipv4.tcp_congestion_control=ndm_tcp
	@echo "NDM-TCP is now the default congestion control algorithm"
	@echo "Available algorithms: $$(sysctl net.ipv4.tcp_available_congestion_control)"

# Disable NDM-TCP (restore to cubic)
disable:
	@echo "Restoring default congestion control..."
	sudo sysctl -w net.ipv4.tcp_congestion_control=cubic
	@echo "Restored to cubic"

# Clean build artifacts
clean:
	@echo "Cleaning build files..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod *.order *.symvers
	@echo "Clean complete"

# Show module info
info:
	@if [ -f ndm_tcp_lkm.ko ]; then \
		echo "Module Information:"; \
		modinfo ndm_tcp_lkm.ko; \
	else \
		echo "Module not built yet. Run 'make' first."; \
	fi

# Show current TCP congestion control settings
status:
	@echo "=== TCP Congestion Control Status ==="
	@echo "Current algorithm: $$(sysctl -n net.ipv4.tcp_congestion_control)"
	@echo "Available algorithms: $$(sysctl -n net.ipv4.tcp_available_congestion_control)"
	@echo ""
	@echo "=== NDM-TCP Module Status ==="
	@if lsmod | grep -q ndm_tcp_lkm; then \
		echo "NDM-TCP module: LOADED"; \
		lsmod | grep ndm_tcp_lkm; \
	else \
		echo "NDM-TCP module: NOT LOADED"; \
	fi

# Quick test (requires iperf3)
test: enable
	@echo "=== NDM-TCP Quick Test ==="
	@echo "This will run a simple loopback test..."
	@echo "Make sure iperf3 is installed: sudo apt-get install iperf3"
	@echo ""
	@echo "Starting iperf3 server in background..."
	@iperf3 -s -D
	@sleep 1
	@echo "Running test with NDM-TCP..."
	@iperf3 -c 127.0.0.1 -t 10 -C ndm_tcp || echo "Test failed - check if iperf3 is installed"
	@pkill iperf3 || true
	@echo ""
	@echo "Check kernel logs for NDM-TCP activity:"
	@dmesg | grep -i ndm | tail -10

# Help target
help:
	@echo "NDM-TCP Kernel Module Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  make              - Build the kernel module"
	@echo "  make install      - Install module to system"
	@echo "  make load         - Load module into running kernel"
	@echo "  make unload       - Unload module from kernel"
	@echo "  make enable       - Set NDM-TCP as default congestion control"
	@echo "  make disable      - Restore default (cubic) congestion control"
	@echo "  make status       - Show current congestion control status"
	@echo "  make info         - Display module information"
	@echo "  make test         - Run basic functionality test"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Example workflow:"
	@echo "  1. make            # Build the module"
	@echo "  2. make load       # Load into kernel"
	@echo "  3. make enable     # Set as default"
	@echo "  4. make status     # Verify it's active"
	@echo "  5. make test       # Run tests (optional)"

.PHONY: all install load unload enable disable clean info status test help