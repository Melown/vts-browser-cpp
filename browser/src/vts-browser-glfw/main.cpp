#include <cstdio>
#include <cstdlib>
#include <vts/map.hpp>
#include <vts/options.hpp>
#include "mainWindow.hpp"
#include "dataThread.hpp"
#include "threadName.hpp"
#include <GLFW/glfw3.h>

void errorCallback(int, const char* description)
{
    fprintf(stderr, "GLFW error: %s\n", description);
}

void usage(char *argv[])
{
    printf("Usage: %s [options] [--] <url> [url]...\n", argv[0]);
}

int main(int argc, char *argv[])
{
    // release build -> catch exceptions and print them to stderr
    // debug build -> let the debugger handle the exceptions
#ifdef NDEBUG
    try
    {
#endif
        //vts::setLogMask("ND");
        //vts::setLogMask("ALL");
        
        int firstUrl = argc;
        for (int i = 1; i < argc; i++)
        {
            if (argv[i][0] != '-')
            {
                firstUrl = i;
                break;
            }
            if (strcmp(argv[i], "--") == 0)
            {
                firstUrl = i + 1;
                break;
            }
            
            // todo handle options
            
            fprintf(stderr, "Unknown option '%s'\n", argv[i]);
            usage(argv);
            return 4;
        }
        if (firstUrl >= argc)
        {
            usage(argv);
            return 3;
        }
    
        glfwSetErrorCallback(&errorCallback);
        if (!glfwInit())
            return 2;
    
        {
            vts::MapCreateOptions options;
            vts::Map map(options);
            map.setMapConfigPath(argv[firstUrl]);
            MainWindow main;
            for (int i = firstUrl; i < argc; i++)
                main.mapConfigPaths.push_back(argv[i]);
            DataThread data(main.window);
            main.map = &map;
            data.map = &map;
            setThreadName("main");
            main.run();
        }
    
        glfwTerminate();
        return 0;
#ifdef NDEBUG
    }
    catch(const std::exception &e)
    {
        fprintf(stderr, "Exception: %s\n", e.what());
        return 1;
    }
    catch(const char *e)
    {
        fprintf(stderr, "Exception: %s\n", e);
        return 1;
    }
    catch(...)
    {
        fprintf(stderr, "Unknown exception.\n");
        return 1;
    }
#endif
}