#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict
import argparse
import os
import sys

# 配置中文字体
plt.rcParams['font.sans-serif'] = ['SimHei', 'DejaVu Sans', 'PingFang SC', 'Microsoft YaHei']
plt.rcParams['axes.unicode_minus'] = False

# ==================== 常量定义 (按原C++精准对齐) ====================
CADU_SIZE = 224
PAYLOAD_OFFSET = 13
PAYLOAD_SIZE = 209
CHECKSUM_SIZE = 2

FINE_TIME_STEPS = 256
TIME_TICK_SECONDS = 254.84
FINE_TIME_STEP = TIME_TICK_SECONDS / FINE_TIME_STEPS

MAG_READING_LEN = 15
MAG_READINGS_PER_FRAME = 13
MAG_ADC_OFFSET = 2339
MAG_SCALE = 2.359
MAG_RAW_SANITY = 10000
VOLTAGE_DIVISOR = 125.0

PARTICLE_READING_LEN = 19
PARTICLE_CHANNELS = 8
PARTICLE_READINGS_PER_FRAME = 11
PARTICLE_MARKER_A0 = 0xA0
PARTICLE_MARKER_A1 = 0xA1

SKIF_ESA_PKT_SIZE = 69
SKIF_ESA_DATA_LEN = 65
SKIF_ESA_BINS = (SKIF_ESA_DATA_LEN - 1) // 2
SKIF_ESA_OFFSETS = [0, 69, 138]

HK_BLK_SIZE = 12
HK_CHANNELS = 5

# ==================== 辅助工具 ====================
def read_be16(data, off=0):
    return int.from_bytes(data[off:off+2], 'big')

def read_be16s(data, off=0):
    v = read_be16(data, off)
    return v - 65536 if v >= 32768 else v

def validate_checksum(cadu):
    return (sum(cadu[:CADU_SIZE - CHECKSUM_SIZE]) & 0xFFFF) == read_be16(cadu, CADU_SIZE - CHECKSUM_SIZE)

def elapsed_seconds(coarse, fine):
    return coarse * TIME_TICK_SECONDS + fine * FINE_TIME_STEP

# ==================== 数据解析器 ====================
class GGAKReader:
    def __init__(self):
        self.total_frames = 0
        self.fill_frames = 0
        self.checksum_pass = 0
        self.checksum_fail = 0
        self.source_id = 0
        self.profile_name = "未知"
        
        # 磁强计
        self.mag_t = []
        self.mag_bt = []
        self.mag_bx = []
        self.mag_by = []
        self.mag_bz = []
        self.mag_v = []

        # 粒子
        self.particle_t = []
        self.particle_ch = [[] for _ in range(PARTICLE_CHANNELS)]

        # ESA
        self.esa_v_t = []
        self.esa_v_bins = []
        self.esa_g_t = []
        self.esa_g_bins = []

        # HK
        self.hk_tsi_t = []
        self.hk_tsi_v = []

        # SER
        self.ser_v_t = []
        self.ser_v_v = []
        self.ser_g_t = []
        self.ser_g_v = []

    def push_frame(self, cadu):
        self.total_frames += 1
        if validate_checksum(cadu):
            self.checksum_pass += 1
        else:
            self.checksum_fail += 1

        # 解析头部
        ft = cadu[4]
        mc = read_be16(cadu, 5)
        cc = read_be16(cadu, 7)
        sid = cadu[9]
        ct = read_be16(cadu, 10)
        ftime = cadu[12]
        abs_t = elapsed_seconds(ct, ftime)

        if ft in (0x77, 0x88):
            self.fill_frames += 1
            return

        if self.source_id == 0:
            self.source_id = sid
            self.profile_name = "GGAK-E" if sid == 0x30 else "GGAK-VE" if sid in (0x31, 0x33) else "GGAK(未知)"

        payload = cadu[PAYLOAD_OFFSET:PAYLOAD_OFFSET+PAYLOAD_SIZE]

        if ft == 0x70:  # FM-VE
            self._decode_mag(payload, abs_t)
        elif ft == 0x40:  # GALS-VE
            self._decode_particle(payload, abs_t)
        elif ft == 0x20:
            self._decode_esa(payload, abs_t, 0x90, 'v')
        elif ft == 0x30:
            self._decode_esa(payload, abs_t, 0x98, 'g')
        elif ft == 0x00:
            self._decode_hk(payload, abs_t, 0x80)
        elif ft == 0x10:
            self._decode_ser(payload, abs_t, 0x88)

    def _decode_mag(self, payload, t):
        for i in range(MAG_READINGS_PER_FRAME):
            off = i * MAG_READING_LEN
            if off + MAG_READING_LEN > len(payload):
                break
            raw_b = read_be16(payload, off)
            raw_x = read_be16(payload, off+2)
            raw_y = read_be16(payload, off+4)
            raw_z = read_be16(payload, off+6)
            raw_v = read_be16(payload, off+8)
            status = payload[off+12]

            if raw_x > MAG_RAW_SANITY or raw_y > MAG_RAW_SANITY or raw_z > MAG_RAW_SANITY or status == 0xFF:
                continue
            self.mag_t.append(t)
            self.mag_bt.append((raw_b - MAG_ADC_OFFSET) * MAG_SCALE)
            self.mag_bx.append((raw_x - MAG_ADC_OFFSET) * MAG_SCALE)
            self.mag_by.append((raw_y - MAG_ADC_OFFSET) * MAG_SCALE)
            self.mag_bz.append((raw_z - MAG_ADC_OFFSET) * MAG_SCALE)
            self.mag_v.append(raw_v / VOLTAGE_DIVISOR)

    def _decode_particle(self, payload, t):
        for i in range(PARTICLE_READINGS_PER_FRAME):
            off = i * PARTICLE_READING_LEN
            if off + PARTICLE_READING_LEN > len(payload) or payload[off] not in (PARTICLE_MARKER_A0, PARTICLE_MARKER_A1):
                continue
            if all(b == 0xAA for b in payload[off:off+PARTICLE_READING_LEN]):
                continue
            self.particle_t.append(t)
            for ch in range(PARTICLE_CHANNELS):
                self.particle_ch[ch].append(read_be16(payload, off + 3 + ch * 2))

    def _decode_esa(self, payload, t, marker, typ):
        for pkt_off in SKIF_ESA_OFFSETS:
            if pkt_off + SKIF_ESA_PKT_SIZE > len(payload):
                break
            sub = payload[pkt_off:pkt_off+SKIF_ESA_PKT_SIZE]
            if sub[0] != marker or all(b == 0xAA for b in sub):
                continue
            bins = [read_be16s(sub, 3 + ch*2) for ch in range(SKIF_ESA_BINS)]
            if typ == 'v':
                self.esa_v_t.append(t)
                self.esa_v_bins.append(bins)
            else:
                self.esa_g_t.append(t)
                self.esa_g_bins.append(bins)

    def _decode_hk(self, payload, t, marker):
        off = 0
        while off + HK_BLK_SIZE <= len(payload):
            block = payload[off:off+HK_BLK_SIZE]
            if block[0] != marker:
                off += 1
                continue
            if all(b == 0xAA for b in block):
                off += HK_BLK_SIZE
                continue
            tsi_raw = read_be16(block, 2 + 2*2)
            self.hk_tsi_t.append(t)
            self.hk_tsi_v.append(tsi_raw * 0.0331)
            off += HK_BLK_SIZE

    def _decode_ser(self, payload, t, marker):
        off = 0
        while off + HK_BLK_SIZE <= len(payload):
            block = payload[off:off+HK_BLK_SIZE]
            if block[0] != marker or all(b == 0xAA for b in block):
                off += 1 if block[0] != marker else HK_BLK_SIZE
                continue
            self.ser_v_t.append(t)
            self.ser_v_v.append(read_be16(block, 2 + 2*2))
            self.ser_g_t.append(t)
            self.ser_g_v.append(read_be16(block, 2 + 4*2))
            off += HK_BLK_SIZE

# ==================== 绘图函数 ====================
def plot_overview(reader, output_file):
    fig, axes = plt.subplots(3, 2, figsize=(18, 15))
    fig.suptitle('GGAK 解码仪器概览', fontsize=20, weight='bold')

    # 1. FM-VE 磁场
    ax1 = axes[0, 0]
    if reader.mag_t:
        t0 = reader.mag_t[0]
        t = [(x - t0) / 60 for x in reader.mag_t]
        ax1.plot(t, reader.mag_bt, label='|B|', lw=1)
        ax1.plot(t, reader.mag_bx, label='Bx', lw=1)
        ax1.plot(t, reader.mag_by, label='By', lw=1)
        ax1.plot(t, reader.mag_bz, label='Bz', lw=1)
        ax1.set_ylabel('nT (初步)', fontsize=12)
        ax1.legend(loc='upper right')
    ax1.set_title('FM-VE 磁场', fontsize=15)
    ax1.grid(alpha=0.3)

    # 2. 粒子
    ax2 = axes[0, 1]
    pnames = ['Ep≥600MeV', 'Ep≥800MeV', 'Ep≥1100MeV', 'Cg-1', 'Cg-2', 'Cg-3', 'Cg-4', 'MIP']
    if reader.particle_t:
        t0 = reader.particle_t[0]
        t = [(x - t0) / 60 for x in reader.particle_t]
        for i in range(PARTICLE_CHANNELS):
            ax2.plot(t, reader.particle_ch[i], label=pnames[i], lw=0.8)
        ax2.set_yscale('log')
        ax2.set_ylim(bottom=1)
        ax2.set_ylabel('平均计数 (对数刻度)', fontsize=12)
        ax2.legend(loc='lower right', ncol=2, fontsize=9)
    ax2.set_title('GALS-VE 和 SKIF-VE MIP 粒子', fontsize=15)
    ax2.grid(alpha=0.3)

    # 3. SKIF-VE/V ESA
    ax3 = axes[1, 0]
    if reader.esa_v_t:
        t0 = reader.esa_v_t[0]
        t = [(x - t0) / 60 for x in reader.esa_v_t]
        data = np.log10(np.array(reader.esa_v_bins).T + 1)
        im = ax3.pcolormesh(t, np.arange(32), data, shading='nearest', cmap='viridis', vmin=0, vmax=4.5)
        fig.colorbar(im, ax=ax3, label='log10(计数+1)')
        ax3.set_yticks([0, 5, 10, 15, 20, 25, 30])
        ax3.set_ylim(32, 0)
        ax3.set_ylabel('包编号', fontsize=12)
    else:
        ax3.text(0.5, 0.5, '无数据', ha='center', va='center')
    ax3.set_title('SKIF-VE/V ESA 能谱', fontsize=15)

    # 4. SKIF-VE/G ESA
    ax4 = axes[1, 1]
    if reader.esa_g_t:
        t0 = reader.esa_g_t[0]
        t = [(x - t0) / 60 for x in reader.esa_g_t]
        data = np.log10(np.array(reader.esa_g_bins).T + 1)
        im = ax4.pcolormesh(t, np.arange(32), data, shading='nearest', cmap='viridis', vmin=0, vmax=4.5)
        fig.colorbar(im, ax=ax4, label='log10(计数+1)')
        ax4.set_yticks([0, 5, 10, 15, 20, 25, 30])
        ax4.set_ylim(32, 0)
        ax4.set_ylabel('包编号', fontsize=12)
    else:
        ax4.text(0.5, 0.5, '无数据', ha='center', va='center')
    ax4.set_title('SKIF-VE/G ESA 能谱', fontsize=15)

    # 5. 平台服务 ISP-2M TSI
    ax5 = axes[2, 0]
    if reader.hk_tsi_t:
        t0 = reader.hk_tsi_t[0]
        t = [(x - t0) / 60 for x in reader.hk_tsi_t]
        # 聚合为误差棒 (0.5分钟为bin宽)
        bins_dict = {}
        for tx, vx in zip(t, reader.hk_tsi_v):
            bi = int(tx / 0.5)
            bins_dict.setdefault(bi, []).append(vx)
        bt = [b * 0.5 for b in sorted(bins_dict.keys())]
        bm = [np.mean(bins_dict[b]) for b in sorted(bins_dict.keys())]
        bs = [np.std(bins_dict[b]) for b in sorted(bins_dict.keys())]

        ax5.errorbar(bt, bm, yerr=bs, fmt='o', label='HK 数据包', color='#1f77b4', markersize=3, capsize=3, elinewidth=1)
        ax5.step(bt, bm, where='mid', label='中值', color='#1f77b4', lw=1.5)
        ax5.set_ylabel('W/m² (初步)', fontsize=12)
        ax5.legend(loc='lower left')
    else:
        ax5.text(0.5, 0.5, '无数据', ha='center', va='center')
    ax5.set_title('平台服务: ISP-2M TSI', fontsize=15)
    ax5.grid(alpha=0.3)

    # 6. SKIF-VE SER 概要
    ax6 = axes[2, 1]
    if reader.ser_v_t or reader.ser_g_t:
        if reader.ser_v_t:
            t0 = reader.ser_v_t[0]
            t = [(x - t0) / 60 for x in reader.ser_v_t]
            ax6.plot(t, reader.ser_v_v, label='skif-ve/v ser', lw=1)
        if reader.ser_g_t:
            if not reader.ser_v_t:
                t0 = reader.ser_g_t[0]
            t = [(x - t0) / 60 for x in reader.ser_g_t]
            ax6.plot(t, reader.ser_g_v, label='skif-ve/g ser', lw=1)
        ax6.set_ylabel('平均计数 (cts/frame)', fontsize=12)
        ax6.legend(loc='upper right')
    else:
        ax6.text(0.5, 0.5, '无数据', ha='center', va='center')
    ax6.set_title('SKIF-VE SER 概要', fontsize=15)
    ax6.grid(alpha=0.3)

    # 统一下排 X 轴
    for ax in axes[2, :]:
        ax.set_xlabel('相对首个活动 CADU 时间 (分钟)', fontsize=12)

    plt.tight_layout()
    fig.subplots_adjust(top=0.93)
    if output_file:
        plt.savefig(output_file, dpi=120)
        print(f"✅ 图像已保存至: {output_file}")
    else:
        plt.show()

# ==================== 主入口 ====================
def main():
    parser = argparse.ArgumentParser(description='GGAK CADU 解析与概览图')
    parser.add_argument('input', nargs='?', help='输入 CADU 文件路径')
    parser.add_argument('-o', '--output', help='保存图像路径')
    args = parser.parse_args()

    path = args.input
    if not path:
        path = input("请输入 GGAK CADU 文件路径: ").strip()
    if not os.path.isfile(path):
        print(f"❌ 文件不存在: {path}")
        sys.exit(1)

    reader = GGAKReader()
    try:
        with open(path, 'rb') as f:
            while True:
                chunk = f.read(CADU_SIZE)
                if len(chunk) < CADU_SIZE:
                    break
                reader.push_frame(chunk)
    except Exception as e:
        print(f"❌ 读取出错: {e}")
        sys.exit(1)

    print("\n" + "=" * 50)
    print(f"GGAK 解码概况")
    print("=" * 50)
    print(f"检测配置 : {reader.profile_name} (Source ID: 0x{reader.source_id:02X})")
    print(f"总帧数   : {reader.total_frames}")
    print(f"有效帧   : {reader.total_frames - reader.fill_frames}")
    print(f"填充帧   : {reader.fill_frames}")
    print(f"校验通过 : {reader.checksum_pass}")
    print(f"校验失败 : {reader.checksum_fail}")
    print(f"磁强计数 : {len(reader.mag_t)}")
    print(f"粒子计数 : {len(reader.particle_t)}")
    print(f"ESA-V 包 : {len(reader.esa_v_t)}")
    print(f"ESA-G 包 : {len(reader.esa_g_t)}")
    print(f"平台服务 : {len(reader.hk_tsi_t)}")
    print(f"SER 计数 : {len(reader.ser_v_t)}")
    print("=" * 50)

    if reader.total_frames == 0:
        print("❌ 未解析到任何 CADU 帧！")
        sys.exit(1)

    print("\n正在生成图表，请稍候...")
    plot_overview(reader, args.output)

if __name__ == '__main__':
    main()