"""setup.py for clipboard-helper."""

from setuptools import find_packages, setup

setup(
    name="clipboard-helper",
    version="1.0.0",
    description="A macOS local clipboard management tool",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    author="Aethersea",
    python_requires=">=3.8",
    packages=find_packages(exclude=["tests*"]),
    entry_points={
        "console_scripts": [
            "clipboard-helper=clipboard_helper.cli:main",
        ],
    },
    classifiers=[
        "Environment :: MacOS X",
        "Operating System :: MacOS",
        "Programming Language :: Python :: 3",
        "Topic :: Utilities",
    ],
)
