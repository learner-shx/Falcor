import os
os.environ["OPENCV_IO_ENABLE_OPENEXR"]="1"
import cv2
import argparse
# tex_root_path = r"D:\data\InvSVBRDF\OpenSVBRDFMaps\gt"

import numpy as np
import pyexr as exr

# combine all images in a folder to a video
# sort by file name
def generate_videos(imgs_path):
    width = 1280
    height = 720
    fps = 60
    size = (width, height)
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')

    videoWriter = cv2.VideoWriter(imgs_path + os.sep + 'video.mp4', fourcc, fps, size)
    for i in range(480):
        img = cv2.imread(imgs_path + os.sep + str(i) + ".png")
        videoWriter.write(img)
    videoWriter.release()

def concat_map_vertical(map_path):

    ax_ay = cv2.imread(os.path.join(map_path,"ax_ay_texture.exr"),cv2.IMREAD_UNCHANGED)[:,:,::-1]
    diff  = cv2.imread(os.path.join(map_path,"pd_texture.exr"),cv2.IMREAD_UNCHANGED)[:,:,::-1]
    #print(diff)
    spec  = cv2.imread(os.path.join(map_path,"ps_texture.exr"),cv2.IMREAD_UNCHANGED)[:,:,::-1]
    normal= cv2.imread(os.path.join(map_path,"normal_texture.exr"),cv2.IMREAD_UNCHANGED)[:,:,::-1]

    tex = np.concatenate( (diff, normal,ax_ay,spec),axis=0)

    tex = tex**(1/2.2)
    tex = tex[:,:,::-1]
    #print(tex.shape)

    cv2.imwrite(os.path.join(map_path, 'tex_vertical.png'), tex*255)

def extract_single_image(imgs_path):
    img = cv2.imread(imgs_path + os.sep + "tex.png", cv2.IMREAD_UNCHANGED)

    baseColor   = img[:, 0:256, ::-1] / 255
    normal      = img[:, 256:512, ::-1] / 255
    roughness   = img[:, 512:768, ::-1] / 255
    specular    = img[:, 768:1024, ::-1] / 255

    baseColor   = baseColor ** 2.2
    normal      = normal
    roughness   = roughness ** 2.2
    specular    = specular ** 2.2

    exr.write(os.path.join(imgs_path, "pd_texture.exr"), baseColor)
    exr.write(os.path.join(imgs_path, "normal_texture.exr"), normal)
    exr.write(os.path.join(imgs_path, "ax_ay_texture.exr"), roughness)
    exr.write(os.path.join(imgs_path, "ps_texture.exr"), specular)

    # baseColor   = img[:, 0:256, :]
    # normal      = img[:, 256:512, :]
    # roughness   = img[:, 512:768, :]
    # specular    = img[:, 768:1024, :]

    # cv2.imwrite(os.path.join(imgs_path,"pd_texture.png"), baseColor)
    # cv2.imwrite(os.path.join(imgs_path,"normal_texture.png"), normal)
    # cv2.imwrite(os.path.join(imgs_path,"ax_ay_texture.png"), roughness)
    # cv2.imwrite(os.path.join(imgs_path,"ps_texture.png"), specular)

# combine two videos
def combine_videos(output_path, video1_path, video2_path):
    video1 = cv2.VideoCapture(video1_path)
    video2 = cv2.VideoCapture(video2_path)
    fps = video1.get(cv2.CAP_PROP_FPS)
    size = (int(video1.get(cv2.CAP_PROP_FRAME_WIDTH)), int(video1.get(cv2.CAP_PROP_FRAME_HEIGHT)))
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')
    videoWriter = cv2.VideoWriter(output_path, fourcc, fps, size)
    while True:
        success1, frame1 = video1.read()
        # success2, frame2 = video2.read()
        if not success1:
            break
        videoWriter.write(frame1)
    while True:
        success2, frame2 = video2.read()
        if not success2:
            break
        videoWriter.write(frame2)
    videoWriter.release()


def main(tex_root_path, map_type):
    # mapPath = r"D:\data\InvSVBRDF\OpenSVBRDFMaps" + os.sep + mapName

    for folder in os.listdir(tex_root_path):
        # check folder is a folder
        if not os.path.isdir(os.path.join(tex_root_path, folder)):
            continue
        # if "fabric0001_resize1024" in folder :
        print(folder)

        if map_type == 1:
            if "tex.png" not in os.listdir(os.path.join(tex_root_path, folder)):
                continue
            extract_single_image(os.path.join(tex_root_path, folder))

        os.makedirs(os.path.join(tex_root_path, folder, "img_rotate_env_map"), exist_ok=True)
        os.makedirs(os.path.join(tex_root_path, folder, "img_rotate_plane"), exist_ok=True)
        os.makedirs(os.path.join(tex_root_path, folder, "img_rotate_env_map"), exist_ok=True)

        N = 0
        # os.system(f"python SVBRDFDemo.py --mapPath {os.path.join(tex_root_path, folder)} --startN {N} --videoType {3} --mapType {map_type}")
        os.system(f"python SVBRDFDemo.py --mapPath {os.path.join(tex_root_path, folder)} --startN {N} --videoType {0} --mapType {map_type}")
        # os.system(f"python SVBRDFDemo.py --mapPath {os.path.join(tex_root_path, folder)} --startN {N} --videoType {2} --mapType {map_type}")

        # combine_videos(os.path.join(tex_root_path, folder), os.path.join(tex_root_path, folder, "video_relight.mp4"), os.path.join(tex_root_path, folder, "video_rotate_plane.mp4"))
        combine_videos(os.path.join(tex_root_path, folder, "video_rotate_plane+rotate_env.mp4"), os.path.join(tex_root_path, folder, "video_rotate_plane.mp4"), os.path.join(tex_root_path, folder, "video_rotate_env_map.mp4"))
        # combine_videos(os.path.join(tex_root_path, folder, "video.mp4"), os.path.join(tex_root_path, folder, "video_relight.mp4"), os.path.join(tex_root_path, folder, "video_rotate_plane+rotate_env_and_cam.mp4"))
        concat_map_vertical(os.path.join(tex_root_path, folder))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Process some integers.')
    parser.add_argument('--tex_root_path', type=str, default=r"D:\data\InvSVBRDF\De18Selected\N=4_real\real_giftbag3")
    parser.add_argument('--map_type', type=int, default=1, help='a string for the type')



    args = parser.parse_args()
    main(args.tex_root_path, args.map_type)

# python SVBRDFDemo.py --mapPath D:\data\InvSVBRDF\De18Maps\N=1_real\real_rubber-pattern\ours --startN 0 --videoType 0 --mapType 1
