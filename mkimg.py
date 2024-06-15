from PIL import Image, ImageDraw, ImageFont
import keyboard
import requests
import time

day_list = ['一','二','三','四','五','六','日']

time_line_1 = '2024年06月15日 星期六'
time_line_2 = '23:59'
show_conten = ''
old_time = now_time = time.localtime()

#向设备发送数据
def Send_Image_To_ESP32():
    img = Image.open("test.png",'r')
    img = img.convert("L")
    #img.show()
    counter = 0
    byte_counter = 0
    temp_hex = "0b"
    base64_content = ""
    for temp in img.getdata():
        if temp >= 0 and temp <= 100:
            temp_hex += "1"
        else:
            temp_hex += "0"
        counter += 1
        if counter == 8:
            temp_hex = hex(eval(temp_hex))
            base64_content += temp_hex
            base64_content += ','
            temp_hex = "0b"
            counter = 0
            byte_counter += 1
    url = f"http://192.168.1.107/input?dat=\"{base64_content}\""
    res = requests.get(url=url)
#向图片添加字体
def image_add_text(img_path, text, left, top, text_color=(255, 0, 0), text_size=13):
    img = Image.open(img_path)
    # 创建一个可以在给定图像上绘图的对象
    draw = ImageDraw.Draw(img)
    # 字体的格式 这里的SimHei.ttf需要有这个字体
    fontStyle = ImageFont.truetype("msyhbd.ttc", text_size, encoding="utf-8")
    # 绘制文本
    draw.text((left, top), text, text_color, font=fontStyle)
    return img

#构建时间字符串
def Make_Time_String(time):
    return '{}年{:02}月{:02}日 星期{}'.format(now_time.tm_year,now_time.tm_mon,now_time.tm_mday,day_list[now_time.tm_wday]),'{:02}:{:02}'.format(now_time.tm_hour,now_time.tm_min)

def Building_Image():
    img_path = 'test.png'
    img = Image.new('RGB',(296,152),(255,255,255))
    img.save(img_path)
    im = image_add_text(img_path, time_line_1, 10, 5, text_color=(0, 0, 0), text_size=20)
    im.save(img_path)
    im = image_add_text(img_path, time_line_2, 40, 20, text_color=(0, 0, 0), text_size=80)
    im.save(img_path)
    im = image_add_text(img_path, show_conten, 5, 115, text_color=(0, 0, 0), text_size=20)
    im = im.transpose(Image.ROTATE_90)
    im.save(img_path)
    #im.show()

def Flush_Other_Image():
    print("请将要刷入的图片更名为test.png，并确保图片的像素为152x296，即宽x高")
    print("如果您已经准备好，请按下Enter键")
    input()
    Send_Image_To_ESP32()

def Change_Show_conten_file():
    fp = open("show_conten.txt",'w',encoding="utf-8")
    fp.writelines(input("要修改成的内容:"))
    fp.close()

if __name__ == "__main__":
    keyboard.add_hotkey("Ctrl+F1",Flush_Other_Image)
    keyboard.add_hotkey("Ctrl+F2",Change_Show_conten_file)
    now_time = time.localtime()
    time_line_1, time_line_2 = Make_Time_String(now_time)
    fp = open("show_conten.txt",'r',encoding="utf-8")
    show_conten = fp.readlines()[0]
    fp.close()
    #print(time_line_1)
    #print(time_line_2)
    #print(show_conten)
    print(f"当前状态为:{time_line_1} -> {time_line_2} -> {show_conten}| ",end="")
    print("如果您想刷入其他图片，请按下Ctrl+F1,如果您想修改自定义内容行，请修改show_conten.txt文件的内容")
    Building_Image()
    Send_Image_To_ESP32()
    while(True):
        now_time = time.localtime()
        time_line_1, time_line_2 = Make_Time_String(now_time)
        #show_conten =         
        if old_time.tm_min != now_time.tm_min:
            #print(time_line_1)
            #print(time_line_2)
            #print(show_conten)
            old_time = now_time
            print(f"当前状态为:{time_line_1} -> {time_line_2} -> {show_conten}| ",end="")
            print("如果您想刷入其他图片，请按下F1,如果您想修改自定义内容行，请修改show_conten.txt文件的内容")            
            Building_Image()
            Send_Image_To_ESP32()