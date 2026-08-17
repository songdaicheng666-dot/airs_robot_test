from setuptools import find_packages, setup
from glob import glob

package_name = 'robot_sport'

setup(
    name=package_name,
    version='0.0.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages',
            ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        ('share/' + package_name, glob("launch/py/*_launch.py")),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='zhq',
    maintainer_email='1325694319@qq.com',
    description='TODO: Package description',
    license='TODO: License declaration',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'robot_sport = robot_sport.robot_sport:main'
        ],
    },
)
