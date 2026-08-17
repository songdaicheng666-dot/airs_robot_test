from setuptools import find_packages, setup

package_name = 'scout_car'

setup(
    name=package_name,
    version='0.0.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages',
            ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='gs',
    maintainer_email='2637778334@qq.com',
    description='TODO: Package description',
    license='TODO: License declaration',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'scout_publisher = scout_car.scout_publisher:main',
            'scout_subscriber = scout_car.scout_subscriber:main',
        ],
    },
)
