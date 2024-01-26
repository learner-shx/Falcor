import os
os.environ["OPENCV_IO_ENABLE_OPENEXR"]="1"
import cv2
import numpy as np
import imageio
# 你的exr图片所在的文件夹
folder_path = 'results'

# 获取文件夹中所有的exr文件
exr_files = [f for f in os.listdir(folder_path) if f.endswith('.exr')]

# print(exr_files)
# 按文件名排序
exr_files.sort()
images = []
for exr in exr_files:
    path = os.path.join(folder_path, exr)

    img = (cv2.imread(path, cv2.IMREAD_ANYCOLOR | cv2.IMREAD_ANYDEPTH)[...,::-1] * 255).astype(np.uint8)
    # print(img.shape)
    images.append(img)

# 读取图片
# images = [imageio.imread(os.path.join(folder_path, f)) for f in exr_files]

# 输出为gif
imageio.mimsave('output.gif', images, 'GIF', duration=0.5)
