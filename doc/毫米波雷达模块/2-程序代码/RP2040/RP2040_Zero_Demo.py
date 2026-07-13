import machine
import utime
import binascii

# 设置软串口的引脚
tx_pin_number = 4  # 替换为实际的引脚
rx_pin_number = 5  # 替换为实际的引脚

tx_pin = machine.Pin(tx_pin_number)
rx_pin = machine.Pin(rx_pin_number)

# 初始化UART对象
soft_uart = machine.UART(1, baudrate=115200, tx=tx_pin, rx=rx_pin)

def send_hex_string(hex_string):
    hex_bytes = binascii.unhexlify(hex_string)
    soft_uart.write(hex_bytes)

def read_serial_data():
    while True:
        if soft_uart.any():
            # 读取串口数据
            data = soft_uart.readline()
            print("Received:", data)
    
    utime.sleep_ms(100) 

if __name__ == "__main__":
    hex_to_send = "FDFCFBFA0800120000006400000004030201"
    
    send_hex_string(hex_to_send)
    
    response = read_serial_data()
    

