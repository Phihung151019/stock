#!/bin/bash

if [ -z "$1" ]; then
    echo "Cách dùng: ./decompile_dtbo.sh <file_dtbo.img>"
    exit 1
fi

DTBO_IMG=$1
OUTPUT_DIR="extracted_dtbo"
DTS_DIR="output_dts"

# Dọn dẹp và tạo mới thư mục
rm -rf $OUTPUT_DIR $DTS_DIR
mkdir -p $OUTPUT_DIR
mkdir -p $DTS_DIR

echo "--- Bước 1: Trích xuất các phân đoạn từ $DTBO_IMG ---"

# Chạy lệnh dump. Lưu ý: Đảm bảo file mkdtboimg.py nằm cùng thư mục hoặc có trong PATH
python3 mkdtboimg.py dump "$DTBO_IMG" -b "$OUTPUT_DIR/dtbo"

# Kiểm tra xem có file nào được trích xuất không
if [ ! "$(ls -A $OUTPUT_DIR)" ]; then
    echo "LỖI: Không trích xuất được file nào. Kiểm tra lại file .img hoặc mkdtboimg.py"
    exit 1
fi

echo "--- Bước 2: Dịch ngược sang .dts ---"

# Duyệt qua tất cả file bắt đầu bằng 'dtbo.' trong thư mục trích xuất
for file in "$OUTPUT_DIR"/dtbo.*; do
    if [ -f "$file" ]; then
        filename=$(basename "$file")
        echo "Đang xử lý: $filename"
        
        # Dịch ngược dùng dtc
        dtc -I dtb -O dts "$file" -o "$DTS_DIR/$filename.dts" 2>/dev/null
    fi
done

echo "--------------------------------------"
echo "Xong! Kiểm tra thư mục: $DTS_DIR"
ls -l $DTS_DIR
