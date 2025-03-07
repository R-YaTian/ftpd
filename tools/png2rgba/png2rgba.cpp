#include <cstring>
#include <png.h>

constexpr auto VERSION = 1.0;
bool swapBR = false;

unsigned char* LoadFromFile(const char *filePath, unsigned int* size)
{
    png_image img;
    unsigned char *pBuffer = NULL;
    memset(&img, 0, sizeof(img));

    img.version = PNG_IMAGE_VERSION;

    if (png_image_begin_read_from_file(&img, filePath))
    {
        img.format = swapBR ? PNG_FORMAT_BGRA : PNG_FORMAT_RGBA;
        pBuffer = new unsigned char[PNG_IMAGE_SIZE(img)];

        if (png_image_finish_read(&img , NULL, pBuffer, 0, NULL)) {
            *size = img.width * img.height * 4;
            png_image_free(&img);
            return pBuffer;
        } else {
            if(pBuffer) {
                delete[] pBuffer;
                pBuffer = NULL;
            }
            png_image_free(&img);
            return NULL;
        }
    }

    return NULL;
}

int main(int argc,char *argv[])
{
    if(argc < 3) {
        printf("png2rgba Version %.1f\n", VERSION);
        printf("Usage: png2rgba pngFile rgbaFile (--swapbr)\n");
        return 1;
    }

    if(argc == 4)
    {
        if(strcmp(argv[3], "--swapbr") == 0) {
            swapBR = true;
        } else {
            printf("Unknown option: %s\n", argv[3]);
            return 1;
        }
    }

    unsigned int fSize = 0;
    unsigned char *pData = LoadFromFile(argv[1], &fSize);
    if(pData != NULL) {
        FILE *fp = fopen(argv[2], "wb");

        if(!fp) {
            printf("Open File: %s Error!\n", argv[2]);
            return 1;
        } else
            fwrite(pData, fSize, 1, fp);

        fclose(fp);
    } else {
        printf("Read File: %s Error!\n", argv[1]);
        return 1;
    }

    delete[] pData;
    return 0;
}
