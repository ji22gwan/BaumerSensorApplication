The OX-CPP-SDK is a set of software libraries and tools to support the integration of the OX into third party applications. 
The SDK is based on the C++ Library "libOXApi.so" which can be easily integrated into own C++ programs. 

The OX C++ SDK consists of the following parts:
- A C++ shared library (libOXApi.so) which provides access to all OX configuration and measurement possibilities.
- The C++ Header files to use the library.
- A example in C++ which demonstrate the OXApi usage.



Linux - LibOxApi_XX-XX-XX.tar.gz:

Folder structure:
- /usr/lib/[platform]/libOXApi.so   The OX C++ SDK main shared library.
- /usr/include                      The OX C++ SDK header files.
- /example             	            A C++ cmake example which demonstrates the usage of the OXApi.

Notes:

OXApi:
	- Only one configuration connection can be established at one time, so if the Webinterface is active, the OXApi will not be able to connect.
   - Your system needs to provide
     - glibc: GLIBC_2.30
     - libstdc++: GLIBCXX_3.4.21
     - libstdc++ ABI: CXXABI_1.3.11
     - libgcc_s: GCC_3.3.1
     - dynamic loader: ld-linux-x86-64.so.2 with at least GLIBC_2.3
   - This is provided by Ubuntu (>= 20.04), Debian (>= 11), RHEL (>= 9) and others
Use SDK example:
    - The SDK files are copied to ~/OxApi/
    - Depending on your platform the correct libOXApi.so is copied to ~/OxApi/usr/lib
    - Create build directory and run build
       mkdir ~/OxApi/build
       cd ~/OxApi/build
       cmake ../example
       cmake --build .
    - Run example
       ./oxapiexamples



Windows Win32 and x64 - LibOxApi_XX-XX-XX.zip:

Folder structure:
- /libs                             The OX C++ SDK main shared library.
- /include/OXApi                    The OX C++ SDK header files.
- /example                          A C++ cmake example which demonstrates the usage of the OXApi.

Notes:

OXApi:
    - OXApi requires Visual Studio 2017 or higher
    - Only one configuration connection can be established at one time, so if the Webinterface is active, the OXApi will not be able to connect.

UDP Streaming:
    - To use UDP Streaming, the desired streams should be activated by the OXApi or the Webinterface.
    - Ensure that the streaming target IP-Address is configured correctly.
    - If more than one OX should stream to the same computer, different ports should be used.
    - Keep in mind that the firewall may block an application from opening an UDP port.

Version history:
- V1.0.0 First release.
- V1.1.0 Bugfix udp streaming, rework example
- V2.0.0 Small API changes, bugfixes
- V2.0.1 bugfixes (udp streamer)
- V2.0.2 Introduces 32 bit values for profile udp stream (OXx200-B20 and OXx200-B02)
- V2.0.10 Bugfix, UdpStreaming::OxStream would not release memory
- V2.0.15 Improve error messages
- V2.0.16 Update linux build dependencies