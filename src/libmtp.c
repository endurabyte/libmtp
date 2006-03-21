#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "libmtp.h"
#include "unicode.h"
#include "ptp.h"
#include "ptp-pack.h"
#include "libptp-endian.h"
#include "libusb-glue.h"

// Forward declarations of local functions
static int send_file_object(LIBMTP_mtpdevice_t *device, 
			    int const fd, uint64_t size,
			    LIBMTP_progressfunc_t const * const callback,
			    void const * const data);
static int delete_item(LIBMTP_mtpdevice_t *device, uint32_t item_id);

// Map this libptp2 single-threaded callback to the LIBMTP callback type
extern Progress_Callback* globalCallback;
static void *single_threaded_callback_data;
static LIBMTP_progressfunc_t *single_threaded_callback;
static Progress_Callback single_threaded_callback_helper;

/**
 * This is a ugly workaround due to limitations in callback set by
 * libptp2...
 */
static int single_threaded_callback_helper(uint32_t sent, uint32_t total) {
  if (single_threaded_callback != NULL) {
    /* 
     * If the callback return anything else than 0, we should interrupt the processing,
     * but currently libptp2 does not offer anything meaningful here so we have to ignore
     * the return value.
     */
    (void) single_threaded_callback(sent, total, single_threaded_callback_data);
  }  
}

/**
 * Initialize the library.
 */
void LIBMTP_Init(void)
{
  return;
}

/**
 * Get the first connected MTP device.
 * @return a device pointer.
 */
LIBMTP_mtpdevice_t *LIBMTP_Get_First_Device(void)
{
  uint8_t interface_number;
  PTPParams *params;
  PTP_USB *ptp_usb;
  PTPStorageIDs storageIDs;
  unsigned storageID = 0;
  PTPDevicePropDesc dpd;
  uint8_t batteryLevelMax = 100;
  uint16_t ret;
  LIBMTP_mtpdevice_t *tmpdevice;

  // Allocate a parameter block
  params = (PTPParams *) malloc(sizeof(PTPParams));
  ptp_usb = (PTP_USB *) malloc(sizeof(PTP_USB));
  ret = connect_first_device(params, ptp_usb, &interface_number);
  
  switch (ret)
    {
    case PTP_CD_RC_CONNECTED:
      printf("Connected to MTP device.\n");
      break;
    case PTP_CD_RC_NO_DEVICES:
      printf("No MTP devices.\n");
      return NULL;
    case PTP_CD_RC_ERROR_CONNECTING:
      printf("Connection error.\n");
      return NULL;
    }

  // get storage ID
  if (ptp_getstorageids (params, &storageIDs) == PTP_RC_OK) {
    if (storageIDs.n > 0)
      storageID = storageIDs.Storage[0];
    free(storageIDs.Storage);
  }
  
  // Make sure there are no handlers
  params->handles.Handler = NULL;
  
  if (ptp_getdeviceinfo(params, &params->deviceinfo) == PTP_RC_OK) {
    printf("Model: %s\n", params->deviceinfo.Model);
    printf("Serial number: %s\n", params->deviceinfo.SerialNumber);
    printf("Device version: %s\n", params->deviceinfo.DeviceVersion);
  } else {
    goto error_handler;
  }
  
  // Get battery maximum level
  if (ptp_getdevicepropdesc(params, PTP_DPC_BatteryLevel, &dpd) != PTP_RC_OK) {
    printf("Unable to retrieve battery max level.\n");
    goto error_handler;
  }
  // if is NULL, just leave as default
  if (dpd.FORM.Range.MaximumValue != NULL) {
    batteryLevelMax = *(uint8_t *)dpd.FORM.Range.MaximumValue;
    printf("Maximum battery level: %d\n", batteryLevelMax);
  }
  ptp_free_devicepropdesc(&dpd);

  // OK everything got this far, so it is time to create a device struct!
  tmpdevice = (LIBMTP_mtpdevice_t *) malloc(sizeof(LIBMTP_mtpdevice_t));
  tmpdevice->interface_number = interface_number;
  tmpdevice->params = (void *) params;
  tmpdevice->ptp_usb = ptp_usb;
  tmpdevice->storage_id = storageID;
  tmpdevice->maximum_battery_level = batteryLevelMax;

  return tmpdevice;
  
  // Then close it again.
 error_handler:
  close_device(ptp_usb, params, interface_number);
  ptp_free_deviceinfo(&params->deviceinfo);
  if (params->handles.Handler != NULL) {
    free(params->handles.Handler);
  }
  return NULL;
}

/**
 * This closes and releases an allocated MTP device.
 * @param device a pointer to the MTP device to release.
 */
void LIBMTP_Release_Device(LIBMTP_mtpdevice_t *device)
{
  PTPParams *params = (PTPParams *) device->params;

  close_device(device->ptp_usb, params, device->interface_number);
  // Free the device info and any handler
  ptp_free_deviceinfo(&params->deviceinfo);
  if (params->handles.Handler != NULL) {
    free(params->handles.Handler);
  }
  free(device);
}

/**
 * This retrieves the model name (often equal to product name) 
 * of an MTP device.
 * @param device a pointer to the device to get the model name for.
 * @return a newly allocated UTF-8 string representing the model name.
 *         The string must be freed by the caller after use. If the call
 *         was unsuccessful this will contain NULL.
 */
char *LIBMTP_Get_Modelname(LIBMTP_mtpdevice_t *device)
{
  char *retmodel = NULL;
  PTPParams *params = (PTPParams *) device->params;
  
  if (params->deviceinfo.Model != NULL) {
    retmodel = strdup(params->deviceinfo.Model);
  }
  return retmodel;
}

/**
 * This retrieves the serial number of an MTP device.
 * @param device a pointer to the device to get the serial number for.
 * @return a newly allocated UTF-8 string representing the serial number.
 *         The string must be freed by the caller after use. If the call
 *         was unsuccessful this will contain NULL.
 */
char *LIBMTP_Get_Serialnumber(LIBMTP_mtpdevice_t *device)
{
  char *retnumber = NULL;
  PTPParams *params = (PTPParams *) device->params;
  
  if (params->deviceinfo.SerialNumber != NULL) {
    retnumber = strdup(params->deviceinfo.SerialNumber);
  }
  return retnumber;
}

/**
 * This retrieves the device version (hardware and firmware version) of an 
 * MTP device.
 * @param device a pointer to the device to get the device version for.
 * @return a newly allocated UTF-8 string representing the device version.
 *         The string must be freed by the caller after use. If the call
 *         was unsuccessful this will contain NULL.
 */
char *LIBMTP_Get_Deviceversion(LIBMTP_mtpdevice_t *device)
{
  char *retversion = NULL;
  PTPParams *params = (PTPParams *) device->params;
  
  if (params->deviceinfo.DeviceVersion != NULL) {
    retversion = strdup(params->deviceinfo.DeviceVersion);
  }
  return retversion;
}


/**
 * This retrieves the owners name of an MTP device.
 * @param device a pointer to the device to get the owner for.
 * @return a newly allocated UTF-8 string representing the owner. 
 *         The string must be freed by the caller after use.
 */
char *LIBMTP_Get_Ownername(LIBMTP_mtpdevice_t *device)
{
  uint16_t *unistring = NULL;
  char *retstring = NULL;
  PTPParams *params = (PTPParams *) device->params;

  if (ptp_getdevicepropvalue(params, 
			     PTP_DPC_DeviceFriendlyName, 
			     (void **) &unistring, 
			     PTP_DTC_UNISTR) != PTP_RC_OK) {
    return NULL;
  }
  // Convert from UTF-16 to UTF-8
  retstring = ucs2_to_utf8(unistring);
  free(unistring);
  return retstring;
}

/**
 * This function finds out how much storage space is currently used
 * and any additional storage information. Storage may be a hard disk
 * or flash memory or whatever.
 * @param device a pointer to the device to get the storage info for.
 * @param total a pointer to a variable that will hold the 
 *        total the total number of bytes available on this volume after
 *        the call.
 * @param free a pointer to a variable that will hold the number of 
 *        free bytes available on this volume right now after the call.
 * @param storage_description a description of the storage. This may 
 *        be NULL after the call even if it succeeded. If it is not NULL, 
 *        it must be freed by the callee after use.
 * @param volume_label a volume label or similar. This may be NULL after the
 *        call even if it succeeded. If it is not NULL, it must be
 *        freed by the callee after use.
 * @return 0 if the storage info was successfully retrieved, any other
 *        value means failure.
 */
int LIBMTP_Get_Storageinfo(LIBMTP_mtpdevice_t *device, uint64_t * const total, 
			uint64_t * const free, char ** const storage_description, 
			char ** const volume_label)
{
  PTPStorageInfo storageInfo;
  PTPParams *params = (PTPParams *) device->params;
  
  if (ptp_getstorageinfo(params, device->storage_id, &storageInfo) != PTP_RC_OK) {
    printf("LIBMTP_Get_Diskinfo(): failed to get disk info\n");
    *total = 0;
    *free = 0;
    *storage_description = NULL;
    *volume_label = NULL;
    return -1;
  }
  *total = storageInfo.MaxCapability;
  *free = storageInfo.FreeSpaceInBytes;
  *storage_description = storageInfo.StorageDescription;
  *volume_label = storageInfo.VolumeLabel;

  return 0;
}


/**
 * This function retrieves the current battery level on the device.
 * @param device a pointer to the device to get the battery level for.
 * @param maximum_level a pointer to a variable that will hold the 
 *        maximum level of the battery if the call was successful.
 * @param current_level a pointer to a variable that will hold the 
 *        current level of the battery if the call was successful.
 * @return 0 if the storage info was successfully retrieved, any other
 *        value means failure.
 */
int LIBMTP_Get_Batterylevel(LIBMTP_mtpdevice_t *device, 
			    uint8_t * const maximum_level, 
			    uint8_t * const current_level)
{
  uint8_t *value = NULL;
  uint16_t ret;
  PTPParams *params = (PTPParams *) device->params;

  ret = ptp_getdevicepropvalue(params, PTP_DPC_BatteryLevel, (void**) &value, PTP_DTC_UINT8);
  if ((ret != PTP_RC_OK) || (value == NULL)) {
    *maximum_level = 0;
    *current_level = 0;
    if (value != NULL) {
      //free(value);
    }
    return -1;
  }
  
  *maximum_level = device->maximum_battery_level;
  *current_level = *value;
  //free(value);
  
  return 0;
}

/**
 * This creates a new track metadata structure and allocates memory
 * for it. Notice that if you add strings to this structure they
 * will be freed by the corresponding <code>LIBMTP_destroy_track_t</code>
 * operation later, so be careful of using strdup() when assigning 
 * strings, e.g.:
 *
 * <pre>
 * LIBMTP_track_t *track = LIBMTP_new_track_t();
 * track->title = strdup(titlestr);
 * ....
 * LIBMTP_destroy_track_t(track);
 * </pre>
 *
 * @return a pointer to the newly allocated metadata structure.
 * @see LIBMTP_destroy_track_t()
 */
LIBMTP_track_t *LIBMTP_new_track_t(void)
{
  LIBMTP_track_t *new = (LIBMTP_track_t *) malloc(sizeof(LIBMTP_track_t));
  if (new == NULL) {
    return NULL;
  }
  new->title = NULL;
  new->artist = NULL;
  new->album = NULL;
  new->genre = NULL;
  new->date = NULL;
  new->filename = NULL;
  new->duration = 0;
  new->tracknumber = 0;
  new->filesize = 0;
  new->codec = LIBMTP_CODEC_UNKNOWN;
  new->next = NULL;
  return new;
}

/**
 * This destroys a track metadata structure and deallocates the memory
 * used by it, including any strings. Never use a track metadata 
 * structure again after calling this function on it.
 * @param track the track metadata to destroy.
 * @see LIBMTP_new_track_t()
 */
void LIBMTP_destroy_track_t(LIBMTP_track_t *track)
{
  if (track == NULL) {
    return;
  }
  if (track->title != NULL)
    free(track->title);
  if (track->artist != NULL)
    free(track->artist);
  if (track->album != NULL)
    free(track->album);
  if (track->genre != NULL)
    free(track->genre);
  if (track->date != NULL)
    free(track->date);
  if (track->filename != NULL)
    free(track->filename);
  free(track);
  return;
}

/**
 * This returns a long list of all tracks available
 * on the current MTP device. Typical usage:
 *
 * <pre>
 * LIBMTP_track_t *tracklist;
 *
 * tracklist = LIBMTP_Get_Tracklisting(device);
 * while (tracklist != NULL) {
 *   LIBMTP_track_t *tmp;
 *
 *   // Do something on each element in the list here...
 *   tmp = tracklist;
 *   tracklist = tracklist->next;
 *   LIBMTP_destroy_track_t(tmp);
 * }
 * </pre>
 *
 * @param device a pointer to the device to get the track listing for.
 * @return a list of tracks that can be followed using the <code>next</code>
 *         field of the <code>LIBMTP_track_t</code> data structure.
 *         Each of the metadata tags must be freed after use, and may
 *         contain only partial metadata information, i.e. one or several
 *         fields may be NULL or 0.
 */
LIBMTP_track_t *LIBMTP_Get_Tracklisting(LIBMTP_mtpdevice_t *device)
{
  uint32_t i = 0;
  LIBMTP_track_t *retracks = NULL;
  LIBMTP_track_t *curtrack = NULL;
  PTPParams *params = (PTPParams *) device->params;
  
  printf("Getting handles\n");
  if (params->handles.Handler == NULL) {
    // Get all the handles if we haven't already done that
    if (ptp_getobjecthandles(params,
			     PTP_GOH_ALL_STORAGE, 
			     PTP_GOH_ALL_FORMATS, 
			     PTP_GOH_ALL_ASSOCS, 
			     &params->handles) != PTP_RC_OK) {
      printf("LIBMTP panic: Could not get object handles...\n");
      return NULL;
    }
  }
  
  for (i = 0; i < params->handles.n; i++) {

    LIBMTP_track_t *track;
    PTPObjectInfo oi;
    int ret;
    char *stringvalue = NULL;
    unsigned short *unicodevalue = NULL;
    uint16_t *uint16value = NULL;
    uint32_t *uint32value = NULL;

    if (ptp_getobjectinfo(params, params->handles.Handler[i], &oi) == PTP_RC_OK) {
      
      // Ignore stuff we don't know how to handle...
      if (oi.ObjectFormat == PTP_OFC_Association || 
	  (oi.ObjectFormat != PTP_OFC_WAV && 
	   oi.ObjectFormat != PTP_OFC_MP3 && 
	   oi.ObjectFormat != PTP_OFC_WMA)) {
	printf("Unknown ObjectFormat (%d), skipping...\n",oi.ObjectFormat);
	continue;
      }
      
      // Allocate a new track type
      track = LIBMTP_new_track_t();

      switch (oi.ObjectFormat)
	{
	case PTP_OFC_WAV:
	  track->codec = LIBMTP_CODEC_WAV;
	  break;
	case PTP_OFC_MP3:
	  track->codec = LIBMTP_CODEC_MP3;
	  break;
	case PTP_OFC_WMA:
	  track->codec = LIBMTP_CODEC_WMA;
	  break;
	default:
	  track->codec = LIBMTP_CODEC_UNKNOWN;
	}

      // Original file-specific properties
      track->filesize = oi.ObjectCompressedSize;
      if (oi.Filename != NULL) {
	track->filename = strdup(oi.Filename);
	printf("Filename: %s\n", track->filename);
      }

      ret = ptp_getobjectpropvalue(params, PTP_OPC_Name, 
				   params->handles.Handler[i], 
				   (void**) &unicodevalue,
				   PTP_DTC_UNISTR);
      if (ret == PTP_RC_OK && unicodevalue != NULL) {
	printf("Getting unicode rep\n");
	track->title = ucs2_to_utf8(unicodevalue);
	free(unicodevalue);
	unicodevalue = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_Artist, 
				   params->handles.Handler[i], 
				   (void**) &unicodevalue, 
				   PTP_DTC_UNISTR);
      if (ret == PTP_RC_OK && unicodevalue != NULL) {
	track->artist = ucs2_to_utf8(unicodevalue);
	free(unicodevalue);
	unicodevalue = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_Duration, 
				   params->handles.Handler[i], 
				   (void**) &uint32value, 
				   PTP_DTC_UINT32);
      if (ret == PTP_RC_OK && uint32value != NULL) {
	track->duration = *uint32value;
	free(uint32value);
	uint32value = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_Track, 
				   params->handles.Handler[i], 
				   (void**) &uint16value, 
				   PTP_DTC_UINT16);
      if (ret == PTP_RC_OK && uint16value != NULL) {
	track->tracknumber = *uint16value;
	free(uint16value);
	uint16value = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_Genre, 
				   params->handles.Handler[i], 
				   (void**) &unicodevalue, 
				   PTP_DTC_UNISTR);
      if (ret == PTP_RC_OK && unicodevalue != NULL) {
	track->genre = ucs2_to_utf8(unicodevalue);
	free(unicodevalue);
	unicodevalue = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_AlbumName, 
				   params->handles.Handler[i], 
				   (void**) &unicodevalue, 
				   PTP_DTC_UNISTR);
      if (ret == PTP_RC_OK && unicodevalue != NULL) {
	track->album = ucs2_to_utf8(unicodevalue);
	free(unicodevalue);
	unicodevalue = NULL;
      }
      
      ret = ptp_getobjectpropvalue(params, 
				   PTP_OPC_OriginalReleaseDate, 
				   params->handles.Handler[i], 
				   (void**) &stringvalue, 
				   PTP_DTC_STR);
      if (ret == PTP_RC_OK && stringvalue != NULL) {
	track->date = strdup(stringvalue);
	free(stringvalue);
	stringvalue = NULL;
      }
      
      // This is some sort of unique ID so we can keep track of the track.
      track->item_id = params->handles.Handler[i];
      
      // Add track to a list that will be returned afterwards.
      if (retracks == NULL) {
	retracks = track;
	curtrack = track;
      } else {
	curtrack->next = track;
	curtrack = track;
      }
      
      // Call listing callback
      // double progressPercent = (double)i*(double)100.0 / (double)params->handles.n;

    } else {
      printf("LIBMTP panic: Found a bad handle, trying to ignore it.\n");
    }

  } // Handle counting loop
  return retracks;
}

/**
 * This gets a track off the device to a file identified
 * by a filename.
 * @param device a pointer to the device to get the track from.
 * @param id the track ID of the track to retrieve.
 * @param path a filename to use for the retrieved track.
 * @param callback a progress indicator function or NULL to ignore.
 * @param data a user-defined pointer that is passed along to
 *             the <code>progress</code> function in order to
 *             pass along some user defined data to the progress
 *             updates. If not used, set this to NULL.
 * @return 0 if the transfer was successful, any other value means 
 *           failure.
 * @see LIBMTP_Get_Track_To_File_Descriptor()
 */
int LIBMTP_Get_Track_To_File(LIBMTP_mtpdevice_t *device, uint32_t const id, 
			 char const * const path, LIBMTP_progressfunc_t const * const callback,
			 void const * const data)
{
  int fd = -1;
  int ret;

  // Sanity check
  if (path == NULL) {
    printf("LIBMTP_Get_Track_To_File(): Bad arguments, path was NULL\n");
    return -1;
  }

  // Open file
#ifdef __WIN32__
  if ( (fd = open(path, O_RDWR|O_CREAT|O_TRUNC|O_BINARY,S_IRWXU|S_IRGRP) == -1 ) {
#else
  if ( (fd = open(path, O_RDWR|O_CREAT|O_TRUNC,S_IRWXU|S_IRGRP)) == -1) {
#endif
    printf("LIBMTP_Get_Track_To_File(): Could not create file \"%s\"\n", path);
    return -1;
  }
  
  ret = LIBMTP_Get_Track_To_File_Descriptor(device, id, fd, callback, data);

  // Close file
  close(fd);
  
  return ret;
}

/**
 * This gets a track off the device to a file identified
 * by a file descriptor.
 * @param device a pointer to the device to get the track from.
 * @param id the track ID of the track to retrieve.
 * @param fd a file descriptor to write the track to.
 * @param callback a progress indicator function or NULL to ignore.
 * @param data a user-defined pointer that is passed along to
 *             the <code>progress</code> function in order to
 *             pass along some user defined data to the progress
 *             updates. If not used, set this to NULL.
 * @return 0 if the transfer was successful, any other value means 
 *           failure.
 * @see LIBMTP_Get_Track_To_File()
 */
int LIBMTP_Get_Track_To_File_Descriptor(LIBMTP_mtpdevice_t *device, 
					uint32_t const id, 
					int const fd, 
					LIBMTP_progressfunc_t const * const callback,
					void const * const data)
{
  PTPObjectInfo oi;
  void *image;
  int ret;
  PTPParams *params = (PTPParams *) device->params;

  single_threaded_callback_data = data;
  single_threaded_callback = callback;
  globalCallback = single_threaded_callback_helper;

  if (ptp_getobjectinfo(params, id, &oi) != PTP_RC_OK) {
    printf("LIBMTP_Get_Track_To_File_Descriptor(): Could not get object info\n");
    return -1;
  }
  if (oi.ObjectFormat == PTP_OFC_Association) {
    printf("LIBMTP_Get_Track_To_File_Descriptor(): Bad object format\n");
    return -1;
  }
  // Seek to end of file and write a blank so that it is created with the
  // correct size and all.
  lseek(fd, oi.ObjectCompressedSize-1, SEEK_SET);
  write(fd, "", 1);

  // MAP_SHARED, MAP_PRIVATE
  image = mmap(0, oi.ObjectCompressedSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (image == MAP_FAILED) {
    printf("LIBMTP_Get_Track_To_File_Descriptor(): Could not map file to memory\n");
    return -1;
  }
  // Flush the file to disk.
  fflush(NULL);
  
  // Copy object to memory
  ret = ptp_getobject(params, id, (char **) &image);

  // Spool out to file
  munmap(image, oi.ObjectCompressedSize);
  
  if (ret != PTP_RC_OK) {
    printf("LIBMTP_Get_Track_To_File_Descriptor(): Could not get file from device\n");
    return -1;
  }

  return 0;
}

/**
 * This function sends a track from a local file to an
 * MTP device. A filename and a set of metadata must be
 * given as input.
 * @param device a pointer to the device to send the track to.
 * @param path the filename of a local file which will be sent.
 * @param metadata a track metadata set to be written along with the file.
 * @param callback a progress indicator function or NULL to ignore.
 * @param data a user-defined pointer that is passed along to
 *             the <code>progress</code> function in order to
 *             pass along some user defined data to the progress
 *             updates. If not used, set this to NULL.
 * @return 0 if the transfer was successful, any other value means 
 *           failure.
 * @see LIBMTP_Send_Track_From_File_Descriptor()
 */
int LIBMTP_Send_Track_From_File(LIBMTP_mtpdevice_t *device, 
			 char const * const path, LIBMTP_track_t * const metadata,
                         LIBMTP_progressfunc_t const * const callback,
			 void const * const data)
{
  int fd;
  int ret;

  // Sanity check
  if (path == NULL) {
    printf("LIBMTP_Send_Track_From_File(): Bad arguments, path was NULL\n");
    return -1;
  }

  // Open file
#ifdef __WIN32__
  if ( (fd = open(path, O_RDONLY|O_BINARY) == -1 ) {
#else
  if ( (fd = open(path, O_RDONLY)) == -1) {
#endif
    printf("LIBMTP_Get_Track_To_File(): Could not open source file \"%s\"\n", path);
    return -1;
  }

  ret = LIBMTP_Send_Track_From_File_Descriptor(device, fd, metadata, callback, data);
  
  // Close file.
  close(fd);

  return ret;
}


/**
 * This is an internal function used by both the file and track 
 * send functions. This takes care of a created object and 
 * transfer the actual file contents to it.
 * @param device a pointer to the device to send the track to.
 * @param fd the filedescriptor for a local file which will be sent.
 * @param size the size of the file to be sent.
 * @param callback a progress indicator function or NULL to ignore.
 * @param data a user-defined pointer that is passed along to
 *             the <code>progress</code> function in order to
 *             pass along some user defined data to the progress
 *             updates. If not used, set this to NULL.
 * @return 0 if the transfer was successful, any other value means 
 *           failure.
 */
static int send_file_object(LIBMTP_mtpdevice_t *device, 
		      int const fd, uint64_t size,
		      LIBMTP_progressfunc_t const * const callback,
		      void const * const data)
{
  PTPContainer ptp;
  PTPUSBBulkContainerSend usbdata;
  uint16_t ret;
  uint8_t *buffer;
  uint64_t remain;
  PTPParams *params = (PTPParams *) device->params;

  // Nullify and configure PTP container
  memset(&ptp, 0, sizeof(ptp));
  ptp.Code = PTP_OC_SendObject;
  ptp.Nparam = 0;
  ptp.Transaction_ID = params->transaction_id++;
  ptp.SessionID = params->session_id;

  // Send request to send an object
  ret = params->sendreq_func(params, &ptp);
  if (ret != PTP_RC_OK) {
    ptp_perror(params, ret);
    printf("LIBMTP_Send_Track_From_File_Descriptor: Could not send \"send object\" request\n");
    return -1;
  }

  // build appropriate USB container
  usbdata.length = htod32p(params,sizeof(usbdata) + size);
  usbdata.type = htod16p(params,PTP_USB_CONTAINER_DATA);
  usbdata.code = htod16p(params,PTP_OC_SendObject);
  usbdata.trans_id = htod32p(params,ptp.Transaction_ID);

  // Write request to USB
  ret = params->write_func((unsigned char *)&usbdata, sizeof(usbdata), params->data);
  if (ret != PTP_RC_OK) {
    printf("LIBMTP_Send_Track_From_File_Descriptor: Error initializing sending object\n");
    ptp_perror(params, ret);
    return -1;
  }
	
  // This space will be used as a reading ring buffer for the transfers
  buffer = (uint8_t *) malloc(BLOCK_SIZE);
  if (buffer == NULL) {
    printf("LIBMTP_Send_Track_From_File_Descriptor: Could not allocate send buffer\n");
    return -1;
  }
	
  remain = size;
  while (remain != 0) {
    int readsize = (remain > BLOCK_SIZE) ? BLOCK_SIZE : (int) remain;
    int bytesdone = (int) (size - remain);
    int readbytes;

    readbytes = read(fd, buffer, readsize);
    if (readbytes < readsize) {
      printf("LIBMTP_Send_Track_From_File_Descriptor: error reading source file\n");
      printf("Wanted to read %d bytes but could only read %d.\n", readsize, readbytes);
      free(buffer);
      return -1;
    }
    
    if (callback != NULL) {
      // If the callback return anything else than 0, interrupt the processing
      int callret = callback(bytesdone, size, data);
      if (callret != 0) {
	printf("LIBMTP_Send_Track_From_File_Descriptor: transfer interrupted by callback\n");
	free(buffer);
	return -1;
      }
    }
    
    // Write to USB
    ret = params->write_func(buffer, readsize, params->data);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Send_Track_From_File_Descriptor: error writing data chunk to object\n");
      ptp_perror(params, ret);
      free(buffer);
      return -1;
    }
    remain -= (uint64_t) readsize;
  }
  
  if (callback != NULL) {
    // This last call will not be able to abort execution and is just
    // done so progress bars go up to 100%
    (void) callback(size, size, data);
  }
  
  // Signal to USB that this is the last transfer if the last chunk
  // was exactly as large as the buffer. This was part of Richards
  // source code but apparently has some problems on Linux for some reason,
  // could be that libusb on Linux intrinsically adds the final zero-length
  // transfer call.
  if (size % MTP_DEVICE_BUF_SIZE == 0) {
    ret = params->write_func(NULL, 0, params->data);
    if (ret!=PTP_RC_OK) {
      printf("LIBMTP_Send_Track_From_File_Descriptor: error writing last zerolen data chunk for USB termination\n");
      ptp_perror(params, ret);
      free(buffer);
      return -1;
    }
  }

  // Get a response from device to make sure that the track was properly stored
  ret = params->getresp_func(params, &ptp);
  if (ret != PTP_RC_OK) {
    printf("LIBMTP_Send_Track_From_File_Descriptor: error getting response from device\n");
    ptp_perror(params, ret);
    free(buffer);
    return -1;
  }

  // Free allocated buffer
  free(buffer);

  return 0;
}
 

/**
 * This function sends a track from a file descriptor to an
 * MTP device. A filename and a set of metadata must be
 * given as input.
 * @param device a pointer to the device to send the track to.
 * @param fd the filedescriptor for a local file which will be sent.
 * @param metadata a track metadata set to be written along with the file.
 *                 After this call the field <code>item_id</code>
 *                 will contain the new track ID.
 * @param callback a progress indicator function or NULL to ignore.
 * @param data a user-defined pointer that is passed along to
 *             the <code>progress</code> function in order to
 *             pass along some user defined data to the progress
 *             updates. If not used, set this to NULL.
 * @return 0 if the transfer was successful, any other value means 
 *           failure.
 * @see LIBMTP_Send_Track_From_File()
 */
int LIBMTP_Send_Track_From_File_Descriptor(LIBMTP_mtpdevice_t *device, 
			 int const fd, LIBMTP_track_t * const metadata,
                         LIBMTP_progressfunc_t const * const callback,
			 void const * const data)
{
  uint32_t parenthandle = 0;
  uint16_t ret;
  uint32_t store = 0;
  int subcall_ret;
  PTPObjectInfo new_track;
  PTPParams *params = (PTPParams *) device->params;
  
  switch (metadata->codec) {
  case LIBMTP_CODEC_WAV:
    new_track.ObjectFormat = PTP_OFC_WAV;
    break;
  case LIBMTP_CODEC_MP3:
    new_track.ObjectFormat = PTP_OFC_MP3;
    break;
  case LIBMTP_CODEC_WMA:
    new_track.ObjectFormat = PTP_OFC_WMA;
    break;
  default:
    printf("LIBMTP_Send_Track_From_File_Descriptor: unknown codec.\n");
    new_track.ObjectFormat = PTP_OFC_Undefined;
  }
  new_track.Filename = metadata->filename;
  new_track.ObjectCompressedSize = metadata->filesize;

  // Create the object
  ret = ptp_sendobjectinfo(params, &store, &parenthandle, &metadata->item_id, &new_track);
  if (ret != PTP_RC_OK) {
    ptp_perror(params, ret);
    printf("LIBMTP_Send_Track_From_File_Descriptor: Could not send object info\n");
    return -1;
  }

  // Call main function to transfer the track
  subcall_ret = send_file_object(device, fd, metadata->filesize, callback, data);
  if (subcall_ret != 0) {
    printf("LIBMTP_Send_Track_From_File_Descriptor: error sending track object\n");
    (void) delete_item(device, metadata->item_id);
    return -1;
  }
    
  // Set track metadata for the new fine track
  subcall_ret = LIBMTP_Update_Track_Metadata(device, metadata);
  if (subcall_ret != 0) {
    printf("LIBMTP_Send_Track_From_File_Descriptor: error setting metadata for new track\n");
    (void) delete_item(device, metadata->item_id);
    return -1;
  }
  
  return 0;
}

/**
 * This function updates the MTP object metadata on a single file
 * identified by an object ID.
 * @param device a pointer to the device to update the track 
 *        metadata on.
 * @param metadata a track metadata set to be written to the file.
 *        notice that the <code>track_id</code> field of the
 *        metadata structure must be correct so that the
 *        function can update the right file. If some properties
 *        of this metadata are set to NULL (strings) or 0
 *        (numerical values) they will be discarded and the
 *        track will not be tagged with these blank values.
 * @return 0 on success, any other value means failure.
 */
int LIBMTP_Update_Track_Metadata(LIBMTP_mtpdevice_t *device, 
				 LIBMTP_track_t const * const metadata)
{
  uint16_t *unistring = NULL;
  uint16_t ret;
  PTPParams *params = (PTPParams *) device->params;

  // Update title
  if (metadata->title != NULL) {
    unistring = utf8_to_ucs2(metadata->title);
    ret = ptp_setobjectpropvalue(params, PTP_OPC_Name, metadata->item_id, unistring, PTP_DTC_UNISTR);
    free(unistring);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track title\n");
      return -1;
    }
  }

  // Update album
  if (metadata->album != NULL) {
    unistring = utf8_to_ucs2(metadata->album);
    ret = ptp_setobjectpropvalue(params, PTP_OPC_AlbumName, metadata->item_id, unistring, PTP_DTC_UNISTR);
    free(unistring);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track album name\n");
      return -1;
    }
  }

  // Update artist
  if (metadata->artist != NULL) {
    unistring = utf8_to_ucs2(metadata->artist);
    ret = ptp_setobjectpropvalue(params, PTP_OPC_Artist, metadata->item_id, unistring, PTP_DTC_UNISTR);
    free(unistring);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track artist name\n");
      return -1;
    }
  }

  // Update genre
  if (metadata->genre != NULL) {
    unistring = utf8_to_ucs2(metadata->genre);
    ret = ptp_setobjectpropvalue(params, PTP_OPC_Genre, metadata->item_id, unistring, PTP_DTC_UNISTR);
    free(unistring);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track genre name\n");
      return -1;
    }
  }

  // Update duration
  if (metadata->duration != 0) {
    ret = ptp_setobjectpropvalue(params, PTP_OPC_Duration, metadata->item_id, &metadata->duration, PTP_DTC_UINT32);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track duration\n");
      return -1;
    }
  }

  // Update track number.
  if (metadata->tracknumber != 0) {
    ret = ptp_setobjectpropvalue(params, PTP_OPC_Track, metadata->item_id, &metadata->tracknumber, PTP_DTC_UINT16);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track tracknumber\n");
      return -1;
    }
  }

  // Update creation datetime
  if (metadata->date != NULL) {
    ret = ptp_setobjectpropvalue(params, PTP_OPC_OriginalReleaseDate, metadata->item_id, metadata->date, PTP_DTC_STR);
    if (ret != PTP_RC_OK) {
      printf("LIBMTP_Update_Track_Metadata(): could not set track release date\n");
      return -1;
    }
  }
  
  // NOTE: File size is not updated, this should not change anyway.
  // neither will we change the filename.
  
  return 0;
}

/**
 * Internal function called to delete tracks and files alike.
 * @param device a pointer to the device to delete the item from.
 * @param item_id the item to delete.
 * @return 0 on success, any other value means failure.
 */
static int delete_item(LIBMTP_mtpdevice_t *device, 
			uint32_t item_id)
{
  int ret;
  PTPParams *params = (PTPParams *) device->params;

  ret = ptp_deleteobject(params, item_id, 0);
  if (ret != PTP_RC_OK) {
    ptp_perror(params, ret);
    printf("delete_item(): could not delete track object\n");    
    return -1;
  }
  return 0;
}

/**
 * This function deletes a single track off the MTP device,
 * identified by an object ID.
 * @param device a pointer to the device to delete the track from.
 * @param item_id the item to delete.
 * @return 0 on success, any other value means failure.
 */
int LIBMTP_Delete_Track(LIBMTP_mtpdevice_t *device, 
			uint32_t item_id)
{
  return delete_item(device, item_id);
}
