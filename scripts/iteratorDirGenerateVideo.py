import os

root_path = "D:\data\InvSVBRDF\De18Selected"

for nAndType in os.listdir(root_path):
    for matName in os.listdir(os.path.join(root_path, nAndType)):
        os.system(f"python generateDemo.py --tex_root_path {os.path.join(root_path, nAndType, matName)} --map_type {1}")
