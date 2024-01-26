import os
import sys
import argparse
import signal
import sys
from glob import glob
from pathlib import Path
import torch
import falcor
import numpy as np
import pyexr as exr
import argparse
from PIL import Image
import io

import cv2

used_map_path = r"D:\data\InvSVBRDF\TmpMaps"
path = r"C:\WorkSpace\Falcor\media\test_scenes\svbrdf.pyscene"
width = 1280
height = 720

light_name = ["pointLight0", "pointLight2", "pointLight3"]
light_radius = dict()
light_radius["pointLight0"] = 12.6
light_radius["pointLight1"] = 2.5
light_radius["pointLight2"] = 12.3
light_radius["pointLight3"] = 6.2

light_theta = dict()
light_theta["pointLight0"] = 0
light_theta["pointLight1"] = 60
light_theta["pointLight2"] =  60
light_theta["pointLight3"] =  -60

light_offset_y = dict()
light_offset_y["pointLight0"] = 5.1
light_offset_y["pointLight1"] = 0.45
light_offset_y["pointLight2"] = 4.5
light_offset_y["pointLight3"] = 0.3

def load_scene(testbed: falcor.Testbed, scene_path: Path, aspect_ratio=1.0):
    flags = (
        falcor.SceneBuilderFlags.DontMergeMaterials
    )
    print("start load scene")
    print(testbed.should_close)
    testbed.load_scene(scene_path, flags)
    print("load scene finished")
    testbed.scene.camera.aspectRatio = aspect_ratio
    testbed.scene.renderSettings.useAnalyticLights = True
    testbed.scene.renderSettings.useEnvLight = True
    return testbed.scene


def create_testbed(reso: (int, int)):
    device_id = 0
    testbed = falcor.Testbed(
        width=reso[0], height=reso[1], create_window=False, gpu=device_id
    )
    testbed.show_ui = False
    testbed.clock.time = 0
    testbed.clock.pause()
    return testbed


def create_passes(testbed: falcor.Testbed, max_bounces: int, use_war: bool):
    # Rendering graph of the WAR differentiable path tracer.
    render_graph = testbed.create_render_graph("PathTracer")
    rt_pass = render_graph.create_pass(
        "PathTracer",
        "PathTracer",
        {'samplesPerPixel': 10}
    )
    v_buffer_rt_pass = render_graph.create_pass(
        "VBufferRT",
        "VBufferRT",
        {
            "samplePattern": "Stratified",
            "sampleCount": 32,
            "useAlphaTest": False,
        },
    )
    accumulate_pass = render_graph.create_pass(
        "AccumulatePass",
        "AccumulatePass",
        {"enabled": True, "precisionMode": "Single"},
    )
    tone_mapper_pass = render_graph.create_pass(
        "ToneMapper",
        "ToneMapper",
        {"autoExposure": False, "exposureCompensation": 0.0},
    )

    render_graph.add_edge("VBufferRT.vbuffer", "PathTracer.vbuffer")
    render_graph.add_edge("VBufferRT.viewW", "PathTracer.viewW")
    render_graph.add_edge("VBufferRT.mvec", "PathTracer.mvec")
    render_graph.add_edge("PathTracer.color", "AccumulatePass.input")
    render_graph.add_edge("AccumulatePass.output", "ToneMapper.src")
    render_graph.mark_output("ToneMapper.dst")


    passes = {
        "rt": rt_pass,
        "v_buffer_rt": v_buffer_rt_pass,
        "tone_mapping": tone_mapper_pass,
    }

    testbed.render_graph = render_graph
    return passes


def render(spp: int, testbed: falcor.Testbed):
    for _ in range(spp):
        testbed.frame()

    img = testbed.render_graph.get_output("ToneMapper.dst").to_numpy()
    img = img.reshape((height, width, 4)) / 255
    # img = img ** (2.2)

    return img

def rotate_env_map(map_path):
    # copy all the maps to the used map path
    maps = glob(map_path + "/*")
    for map in maps:
        map_name = map.split("\\")[-1]
        print(map_name)
        if map_name[-4:] == ".exr":
            new_map_path = os.path.join(used_map_path, map_name)
            os.system("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')

    testbed = create_testbed((width, height))
    passes = create_passes(testbed, 10, False)

    scene = load_scene(testbed, path, width / height)

    center = [0.0, 0.8, 0.0]
    radius = 0.4


    frames = []
    # change the light position
    N = 360
    # N =6
    for i in range(N):
        # print process
        print(f"rotate_env_map: {i}/{N}")

        # change the light position
        theta = i / N * 2 * np.pi

        angle = - np.sin(theta) * 45

        angle += 90

        testbed.scene.envMap.rotation = [0.0, 125.4 - 90 - angle, 0.0]

        # testbed.scene.camera.position = [center[0] + radius * np.cos(angle / 180 * np.pi), center[1], center[2] + radius * np.sin(angle / 180 * np.pi)]

        for light in light_name:
            angle += light_theta[light]
            testbed.scene.getLight(light).position = [
                center[0] + light_radius[light] * np.cos(angle / 180 * np.pi),
                center[1] + light_offset_y[light],
                center[2] + light_radius[light] * np.sin(angle / 180 * np.pi)]
        # render the scene
        img = render(200, testbed)
        # save the image
        img = img * 255
        img = img.astype(np.uint8)
        cv2.imwrite(map_path + os.sep + "img_rotate_env_map/" + str(i) + ".png", img)

        frames.append(img[..., :3])

    # generate the video
    fps = N // 6
    size = (width, height)
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')

    videoWriter = cv2.VideoWriter(map_path + os.sep + 'video_rotate_env_map.mp4', fourcc, fps, size)
    for frame in frames:
        videoWriter.write(frame)
    videoWriter.release()



def rotate_env_map_and_camera(map_path):
    # copy all the maps to the used map path
    maps = glob(map_path + "/*")
    for map in maps:
        map_name = map.split("\\")[-1]
        if map_name[-4:] == ".exr":
            new_map_path = os.path.join(used_map_path, map_name)
            print("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')
            os.system("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')

    testbed = create_testbed((width, height))
    passes = create_passes(testbed, 10, False)

    scene = load_scene(testbed, path, width / height)

    testbed.scene.camera.target = [0.0, 0.8, 0.3]
    center = [0.0, 0.8, 0.0]
    radius = 1.2


    # camera.position = float3(0, 0.8, 0.575)
    frames = []
    # change the light position
    N = 240
    for i in range(N):
        # print process
        print(f"rotate_env_map_and_camera: {i}/{N}")

        # change the light position
        angle = 0

        step = 60 / N

        stage = N / 4
        if i < stage:
            angle = (i * step)
        elif i < 2 * stage:
            angle = 15 - (i - stage) * step
        elif i < 3 * stage:
            angle = - (i - 2 * stage) * step
        else:
            angle = - 15 + (i - 3 * stage) * step
        angle_y = angle
        angle += 90

        testbed.scene.envMap.rotation = [0.0, 125.4 - 90 - angle, 0.0]

        testbed.scene.camera.position = [center[0] + radius * np.cos(angle / 180 * np.pi), center[1], center[2] + radius * np.sin(angle / 180 * np.pi)]

        for light in light_name:
            angle = 180 - angle
            angle += light_theta[light]
            testbed.scene.getLight(light).position = [
                center[0] + light_radius[light] * np.cos(angle / 180 * np.pi),
                center[1] + light_offset_y[light],
                center[2] + light_radius[light] * np.sin(angle / 180 * np.pi)]
        # render the scene
        img = render(100, testbed)
        # save the image
        img = img * 255
        img = img.astype(np.uint8)
        cv2.imwrite(map_path + os.sep + "img_rotate_env_map_and_cam/" + str(i) + ".png", img)

        frames.append(img[..., :3])

    # generate the video
    fps = N // 4
    size = (width, height)
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')

    videoWriter = cv2.VideoWriter(map_path + os.sep + 'video_rotate_env_map_and_cam.mp4', fourcc, fps, size)
    for frame in frames:
        videoWriter.write(frame)
    videoWriter.release()


def rotate_plane(map_path):
    # copy all the maps to the used map path
    maps = glob(map_path + "/*")
    for map in maps:
        map_name = map.split("\\")[-1]
        if map_name[-4:] == ".exr":
            new_map_path = os.path.join(used_map_path, map_name)
            print("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')
            os.system("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')

    testbed = create_testbed((width, height))
    passes = create_passes(testbed, 10, False)

    scene = load_scene(testbed, path, width / height)

    testbed.scene.camera.target = [0.0, 0.8, 0.0]

    center = [0.0, 0.8, 0.0]
    radius = 1.2

    # camera.position = float3(0, 0.8, 0.575)
    frames = []
    # change the light position
    N = 240
    # N =6

    for i in range(N):
        # print process
        print(f"rotate_plane: {i}/{N}")
        # change the light position
        angle = 0

        step = 120 / N

        stage = N / 4
        if i < stage:
            angle = (i * step)
        elif i < 2 * stage:
            angle = 30 - (i - stage) * step
        elif i < 3 * stage:
            angle = - (i - 2 * stage) * step
        else:
            angle = - 30 + (i - 3 * stage) * step

        theta = i / N * 2 * np.pi

        angle = np.sin(theta) * 30
        angle += 90.0

        testbed.scene.envMap.rotation = [0.0, 125.4 - 90 - angle, 0.0]

        testbed.scene.camera.position = [center[0] + radius * np.cos(angle / 180 * np.pi), center[1], center[2] + radius * np.sin(angle / 180 * np.pi)]

        for light in light_name:
            angle = angle
            angle += light_theta[light]
            testbed.scene.getLight(light).position = [
                center[0] + light_radius[light] * np.cos(angle / 180 * np.pi),
                center[1] + light_offset_y[light],
                center[2] + light_radius[light] * np.sin(angle / 180 * np.pi)]

        # render the scene
        img = render(200, testbed)
        # save the image
        img = img * 255
        img = img.astype(np.uint8)
        cv2.imwrite(map_path + os.sep + "img_rotate_plane/" + str(i) + ".png", img)

        frames.append(img[..., :3])

    # generate the video
    fps = N // 4
    size = (width, height)
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')

    videoWriter = cv2.VideoWriter(map_path + os.sep + 'video_rotate_plane.mp4', fourcc, fps, size)
    for frame in frames:
        videoWriter.write(frame)
    videoWriter.release()



# change the light position and relight the scene and generate a video
def relight_video(map_path):

    # copy all the maps to the used map path
    maps = glob(map_path + "/*")
    for map in maps:
        map_name = map.split("\\")[-1]
        if map_name[-4:] == ".exr":
            new_map_path = os.path.join(used_map_path, map_name)
            os.system("copy " + '"' + map + '"' + " " + '"' +new_map_path + '"')

    testbed = create_testbed((width, height))
    passes = create_passes(testbed, 10, False)

    scene = load_scene(testbed, path, width / height)

    center = [0.0, 0.8, 0.0]
    radius = 0.4

    frames = []
    # change the light position
    N = 240
    angle = 90.0
    testbed.scene.envMap.rotation = [0.0, - angle, 0.0]
    for i in range(N):
        print(f"relight: {i}/{N}")
        # change the light position
        angle = 0
        angle_y = 0
        step = 240 / N

        stage = N / 4 # 60
        if i < stage:
            angle = (i * step)
        elif i < 2 * stage:
            angle = 60 - (i - stage) * step
        elif i < 3 * stage:
            angle = - (i - 2 * stage) * step
        else:
            angle = - 60 + (i - 3 * stage) * step

        angle += 90

        angle_y = i / 240 * 720


        testbed.scene.getLight("pointLight0").position = [
            center[0] + radius * np.cos(angle / 180 * np.pi),
            center[1] + radius * np.sin(angle_y / 180 * np.pi),
            center[2] + radius * 0.5]
        # render the scene
        img = render(100, testbed)
        # save the image
        img = img * 255
        img = img.astype(np.uint8)
        cv2.imwrite(map_path + os.sep + "img_relight/" + str(i) + ".png", img)

        frames.append(img[..., :3])

    # generate the video
    fps = N // 4
    size = (width, height)
    fourcc = cv2.VideoWriter_fourcc('m', 'p', '4', 'v')

    videoWriter = cv2.VideoWriter(map_path + os.sep + 'video_relight.mp4', fourcc, fps, size)
    for frame in frames:
        print(frame.shape)
        videoWriter.write(frame)
    videoWriter.release()



def main(mapPath, startN, videoType, mapType):
    # mapPath = r"D:\data\InvSVBRDF\OpenSVBRDFMaps" + os.sep + mapName

    OpenSVBRDFSceneName = "svbrdf_OpenSVBRDF.pyscene"
    De18SceneName = "svbrdf_De18.pyscene"
    SceneName = ""

    # OpenSVBRDF
    if mapType == 0:
        SceneName = OpenSVBRDFSceneName
    else:
        SceneName = De18SceneName

    os.system("copy " + "\\".join(path.split("\\")[:-1]) + os.sep + SceneName + " " + path)

    if videoType == 0:
        rotate_plane(map_path=mapPath)
    elif videoType == 1:
        rotate_env_map_and_camera(map_path=mapPath)
    elif videoType == 2:
        rotate_env_map(map_path=mapPath)
    elif videoType == 3:
        relight_video(map_path=mapPath)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Process some integers.')
    parser.add_argument('--mapPath', type=str, default="lether")
    parser.add_argument('--startN', type=int, default=0, help='a string for the type')
    parser.add_argument('--videoType', type=int, default=0, help='a string for the type')
    parser.add_argument('--mapType', type=int, default=0, help='a string for the type')


    args = parser.parse_args()
    main(args.mapPath, args.startN, args.videoType, args.mapType)
