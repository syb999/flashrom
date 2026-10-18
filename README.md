# Flashrom CH341A GUI

CH341A USB SPI 编程器的图形界面（纯 C + X11/Xft，无 GTK/Qt 依赖，静态链接，任意
x86_64 Linux / Linux Mint 17+ 直接运行）。

## 功能

* 检测芯片（通用探测失败时自动按候选型号逐个指定 `-c` 探测）
* 备份整片 / 写入镜像 / 校验 / 整片擦除
* **只写 u-boot 区**：读整片 → 校验读全（读不全立即中止）→ 只替换前 N 个 4K 块
  （N 由 u-boot 文件实际大小决定：64K/128K/192K/256K 均可）→ 整体写回，
  其余区域保持原数据
* 生成空白镜像（全 0xFF + u-boot@0，按检测到的芯片容量；会清空其余区域）
* 内置文件浏览器（双击进目录、可手输路径、可过滤只显示 .bin/.img/.rom）
* 日志可拖动选中、Ctrl+C / 复制选中 / 全选 / 复制全部
* **权限只问一次**：启动时 pkexec 授权一次 → 常驻特权助手执行后续命令
  （装了 udev 规则则完全不需要授权）
* 支持任意 flashrom 编程器：`ch341a_spi`、`ch347_spi`、
  `serprog:dev=/dev/ttyUSB0:2000000`、`linux_spi:dev=/dev/spidev0.0` 等
  （界面上有「编程器列表」按钮，等同于 `flashrom --list-supported`）

> CH340 是 USB 转串口芯片，**不能**直接刷 SPI；要刷需配合带 serprog 固件的设备。

## 构建

```sh
./build.sh          # 需要 meson/ninja、libusb-1.0-dev、libcrypto、Xft 等开发包
                    # 产物: ./flashrom  ./flashrom-gui-c
./make-deb.sh 2.1   # 打包 .deb（含 udev 规则、桌面图标、菜单项、README）
```

## 目录

```
flashrom-gui-c.c         GUI 主程序（自绘 X11 + Xft 界面，UTF-8 中文）
flashrom-helper.sh       pkexec 拉起的常驻特权助手（通过 FIFO 收发命令）
99-ch341a-nopass.rules   CH341A(1a86:5512) 免 root udev 规则
build.sh / make-deb.sh   构建与打包脚本
flashrom/...             上游 flashrom 源码（meson 构建）
```

## 安装（deb）

```sh
sudo dpkg -i flashrom-ch341a-gui_2.1_amd64.deb
# 缺依赖时: sudo apt-get -f install
```

装完：应用菜单 / 桌面图标「Flashrom 编程器 (CH341A)」，或命令行
`flashrom-ch341a-gui`。

配置文件 `~/.config/flashrom-gui.conf`（退出时自动保存路径/芯片/字体/pkexec）。
