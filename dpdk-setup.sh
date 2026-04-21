#!/bin/bash

# 加载驱动
echo "加载vfio驱动..."
sudo modprobe vfio-pci iommu=on

#设置大页内存
echo "设置大页内存..."
sudo dpdk-hugepages.py -p 1G --setup 2G

# 绑定设备到dpdk驱动
echo "绑定设备到dpdk驱动..."
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.0 --force
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.1 --force

# 输出设备状态
sudo dpdk-devbind.py --status-dev net
