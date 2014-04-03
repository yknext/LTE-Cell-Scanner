// Copyright 2012 Evrytania LLC (http://www.evrytania.com)
//
// Written by James Peroulas <james@evrytania.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Improved by Jiao Xianjun (putaoshu@gmail.com):
// 1. TD-LTE support
// 2. OpenCL to speedup
// 3. fast pre-search frequencies (external mixer/LNB support)
// 4. multiple tries at one frequency
// 5. .bin file recording and replaying

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <itpp/itbase.h>
#include <boost/math/special_functions/gamma.hpp>
#include <list>
#include <sstream>
#include <curses.h>
#include <sys/time.h>
#include "rtl-sdr.h"
#include "common.h"
#include "macros.h"
#include "lte_lib.h"
#include "constants.h"
#include "capbuf.h"
#include "filter_coef.h"
#include "itpp_ext.h"
#include "searcher.h"
#include "dsp.h"
#include "rtl-sdr.h"

using namespace itpp;
using namespace std;

uint8 verbosity=1;

// Simple usage screen.
void print_usage() {
  cout << "LTE CellSearch v" << MAJOR_VERSION << "." << MINOR_VERSION << "." << PATCH_LEVEL << " (" << BUILD_TYPE << ") help screen" << endl << endl;
  cout << "CellSearch -s start_frequency [optional_parameters]" << endl;
  cout << "  Basic options" << endl;
  cout << "    -h --help" << endl;
  cout << "      print this help screen" << endl;
  cout << "    -v --verbose" << endl;
  cout << "      increase status messages from program" << endl;
  cout << "    -b --brief" << endl;
  cout << "      reduce status messages from program" << endl;
  cout << "    -i --device-index N" << endl;
  cout << "      specify which attached RTLSDR dongle to use" << endl;
  cout << "    -a --opencl-platform N" << endl;
  cout << "      specify which OpenCL platform to use (default: 0)" << endl;
  cout << "    -j --opencl-device N" << endl;
  cout << "      specify which OpenCL device of selected platform to use (default: 0)" << endl;
  cout << "    -w --filter-workitem N" << endl;
  cout << "      specify how many OpenCL workitems are used for the 1st dim of 6RB filter (you'd better use values like 2^n)" << endl;
  cout << "    -u --xcorr-workitem N" << endl;
  cout << "      specify how many OpenCL workitems are used for the PSS xcorr (you'd better use values like 2^n)" << endl;
  cout << "  Frequency search options:" << endl;
  cout << "    -s --freq-start fs" << endl;
  cout << "      frequency where cell search should start" << endl;
  cout << "    -e --freq-end fe" << endl;
  cout << "      frequency where cell search should end" << endl;
  cout << "    -n --num-try nt" << endl;
  cout << "      number of tries at each frequency/file (default: 1)" << endl;
  cout << "    -m --num-reserve N" << endl;
  cout << "      number of reserved frequency-ppm peak pairs in pre-search phase (default: 1)" << endl;
  cout << "  Dongle LO correction options:" << endl;
  cout << "    -t --twisted" << endl;
  cout << "      enable original sampling-carrier-twisted mode (default is disable and using carrier&sampling isolated pre-search to support external mixer/LNB)" << endl;
  cout << "    -p --ppm ppm" << endl;
  cout << "      crystal remaining PPM error" << endl;
  cout << "    -c --correction c" << endl;
  cout << "      crystal correction factor" << endl;
  cout << "  Capture buffer save/ load options:" << endl;
  cout << "    -z --recbin" << endl;
  cout << "      save captured data in the bin file. (only supports single frequency scanning)" << endl;
  cout << "    -y --loadbin" << endl;
  cout << "      used data in captured bin file. (only supports single frequency scanning)" << endl;
  cout << "    -r --record" << endl;
  cout << "      save captured data in the files capbuf_XXXX.it" << endl;
  cout << "    -l --load" << endl;
  cout << "      used data in capbuf_XXXX.it files instead of live data" << endl;
  cout << "    -d --data-dir dir" << endl;
  cout << "      directory where capbuf_XXXX.it files are located" << endl << endl;
  cout << "'c' is the correction factor to apply and indicates that if the desired" << endl;
  cout << "center frequency is fc, the RTL-SDR dongle should be instructed to tune" << endl;
  cout << "to freqency fc*c so that its true frequency shall be fc. Default: 1.0" << endl << endl;
  cout << "'ppm' is the remaining frequency error of the crystal. Default: 120" << endl;
  cout << "" << endl;
  cout << "If the crystal has not been used for a long time, use the default values for" << endl;
  cout << "'ppm' and 'c' until a cell is successfully located. The program will return" << endl;
  cout << "a 'c' value that can be used in the future, assuming that the crystal's" << endl;
  cout << "frequency accuracy does not change significantly." << endl;
  cout << "" << endl;
  cout << "Even if a correction factor has been calculated, there is usually some" << endl;
  cout << "remaining frequency error in the crystal. Thus, although after a c value is" << endl;
  cout << "calculated the ppm value can be reduced, it can not be reduced to 0." << endl;
  cout << "10.0 is a reasiable ppm value to use after a correction factor has been" << endl;
  cout << "calculated." << endl;
}

// Parse the command line arguments and return optional parameters as
// variables.
// Also performs some basic sanity checks on the parameters.
void parse_commandline(
  // Inputs
  const int & argc,
  char * const argv[],
  // Outputs
  double & freq_start,
  double & freq_end,
  uint16 & num_try,
  bool & sampling_carrier_twist,
  double & ppm,
  double & correction,
  bool & save_cap,
  bool & use_recorded_data,
  string & data_dir,
  int & device_index,
  char * record_bin_filename,
  char * load_bin_filename,
  uint16 & opencl_platform,
  uint16 & opencl_device,
  uint16 & filter_workitem,
  uint16 & xcorr_workitem,
  uint16 & num_reserve,
  uint16 & num_loop
) {
  // Default values
  freq_start=-1;
  freq_end=-1;
  num_try=1; // default number
  sampling_carrier_twist=false;
  ppm=120;
  correction=1;
  save_cap=false;
  use_recorded_data=false;
  data_dir=".";
  device_index=-1;
  opencl_platform = 0;
  opencl_device = 0;
  filter_workitem = 32;
  xcorr_workitem = 2;
  num_reserve = 1;
  num_loop = 0;

  while (1) {
    static struct option long_options[] = {
      {"help",         no_argument,       0, 'h'},
      {"verbose",      no_argument,       0, 'v'},
      {"brief",        no_argument,       0, 'b'},
      {"freq-start",   required_argument, 0, 's'},
      {"freq-end",     required_argument, 0, 'e'},
      {"num-try",      required_argument, 0, 'n'},
      {"twisted",      no_argument,       0, 't'},
      {"ppm",          required_argument, 0, 'p'},
      {"correction",   required_argument, 0, 'c'},
      {"recbin",       required_argument, 0, 'z'},
      {"loadbin",      required_argument, 0, 'y'},
      {"record",       no_argument,       0, 'r'},
      {"load",         no_argument,       0, 'l'},
      {"data-dir",     required_argument, 0, 'd'},
      {"device-index", required_argument, 0, 'i'},
      {"opencl-platform", required_argument, 0, 'a'},
      {"opencl-device", required_argument, 0, 'j'},
      {"filter-workitem", required_argument, 0, 'w'},
      {"xcorr-workitem", required_argument, 0, 'u'},
      {"num-reserve", required_argument, 0, 'm'},
      {"num-loop", required_argument, 0, 'k'},
      {0, 0, 0, 0}
    };
    /* getopt_long stores the option index here. */
    int option_index = 0;
    int c = getopt_long (argc, argv, "hvbs:e:n:tp:c:z:y:rld:i:a:j:w:u:m:k:",
                     long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1)
      break;

    switch (c) {
      char * endp;
      case 0:
        // Code should only get here if a long option was given a non-null
        // flag value.
        cout << "Check code!" << endl;
        ABORT(-1);
        break;
      case 'h':
        print_usage();
        ABORT(-1);
        break;
      case 'v':
        verbosity=2;
        break;
      case 'b':
        verbosity=0;
        break;
      case 's':
        freq_start=strtod(optarg,&endp);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse start frequency" << endl;
          ABORT(-1);
        }
        break;
      case 'e':
        freq_end=strtod(optarg,&endp);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse end frequency" << endl;
          ABORT(-1);
        }
        break;
      case 'n':
        num_try=strtol(optarg,&endp,10);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse number of tries" << endl;
          ABORT(-1);
        }
        break;
      case 't':
        sampling_carrier_twist=true;
        break;
      case 'p':
        ppm=strtod(optarg,&endp);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse ppm value" << endl;
          ABORT(-1);
        }
        break;
      case 'c':
        correction=strtod(optarg,&endp);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse correction factor" << endl;
          ABORT(-1);
        }
        break;
      case 'r':
        save_cap=true;
        break;
      case 'l':
        use_recorded_data=true;
        break;
      case 'z':
        {
          int len_str = strlen(optarg);
          if (len_str<5)
          {
            cerr << "Error: record bin filename too short" << endl;
            ABORT(-1);
          }
          if ( (len_str<1) || (strcmp(optarg+len_str-4, ".bin")) )
          {
            cerr << "Error: could not parse record bin filename (must be .bin file)" << endl;
            ABORT(-1);
          }
          else
          {
            strcpy(record_bin_filename, optarg);
          }
        }
        break;
      case 'y':
        {
          int len_str = strlen(optarg);
          if (len_str<5)
          {
            cerr << "Error: load bin filename too short" << endl;
            ABORT(-1);
          }
          if ( (len_str<1) || (strcmp(optarg+len_str-4, ".bin")) )
          {
            cerr << "Error: could not parse load bin filename (must be .bin file)" << endl;
            ABORT(-1);
          }
          else
          {
            strcpy(load_bin_filename, optarg);
            //freq_start=9999e6; // fake
          }
        }
        break;
      case 'd':
        data_dir=optarg;
        break;
      case 'i':
        device_index=strtol(optarg,&endp,10);
        if ((optarg==endp)||(*endp!='\0')) {
          cerr << "Error: could not parse device index" << endl;
          ABORT(-1);
        }
        if (device_index<0) {
          cerr << "Error: device index cannot be negative" << endl;
          ABORT(-1);
        }
        break;
      case 'a':
        opencl_platform=strtol(optarg,&endp,10);
        break;
      case 'j':
        opencl_device=strtol(optarg,&endp,10);
        break;
      case 'w':
        filter_workitem=strtol(optarg,&endp,10);
      case 'u':
        xcorr_workitem=strtol(optarg,&endp,10);
        break;
      case 'm':
        num_reserve=strtol(optarg,&endp,10);
        break;
      case 'k':
        num_loop = strtol(optarg,&endp,10);
        break;
      case '?':
        /* getopt_long already printed an error message. */
        ABORT(-1);
      default:
        ABORT(-1);
    }
  }

  // Error if extra arguments are found on the command line
  if (optind < argc) {
    cerr << "Error: unknown/extra arguments specified on command line" << endl;
    ABORT(-1);
  }

  // Second order command line checking. Ensure that command line options
  // are consistent.
  if (freq_start==-1) {
//    if (!sampling_carrier_twist) {
      freq_start=9999e6; // fake
      cout << "Warning: Frequency not specified. Make sure you are working on captured file.\n";
//    } else {
//      cerr << "Error: must specify a start frequency. (Try --help)" << endl;
//      ABORT(-1);
//    }
  }
  // Start and end frequencies should be on a 100kHz raster.
  if (freq_start<1e6) {
    cerr << "Error: start frequency must be greater than 1MHz" << endl;
    ABORT(-1);
  }
  // Number of tries should be not less than 1.
  if (num_try<1) {
    cerr << "Error: number of tries at each frequency/file should be not less than 1" << endl;
    ABORT(-1);
  }
  if (freq_start/100e3!=itpp::round(freq_start/100e3)) {
    freq_start=itpp::round(freq_start/100e3)*100e3;
    cout << "Warning: start frequency has been rounded to the nearest multiple of 100kHz" << endl;
  }
  if (freq_end==-1) {
    freq_end=freq_start;
  }
  if (freq_end<freq_start) {
    cerr << "Error: end frequency must be >= start frequency" << endl;
    ABORT(-1);
  }
  if (freq_end/100e3!=itpp::round(freq_end/100e3)) {
    freq_end=itpp::round(freq_end/100e3)*100e3;
    cout << "Warning: end frequency has been rounded to the nearest multiple of 100kHz" << endl;
  }
  // PPM values should be positive an most likely less than 200 ppm.
  if (ppm<0) {
    cerr << "Error: ppm value must be positive" << endl;
    ABORT(-1);
  }
  if (ppm>200) {
    cout << "Warning: ppm value appears to be set unreasonably high" << endl;
  }
  // Warn if correction factor is greater than 1000 ppm.
  if (abs(correction-1)>1000e-6) {
    cout << "Warning: crystal correction factor appears to be unreasonable" << endl;
  }
  // Should never both read and write captured data from a file
  if (save_cap&&use_recorded_data) {
    cerr << "Error: cannot read and write captured data at the same time!" << endl;
    ABORT(-1);
  }
  // Should never both read from .it and read from .bin file.
  if ( use_recorded_data && (strlen(load_bin_filename)>4) ) {
    cerr << "Error: cannot read from .it and .bin file at the same time!" << endl;
    ABORT(-1);
  }

  if (verbosity>=1) {
    cout << "OpenCL LTE CellSearch v" << MAJOR_VERSION << "." << MINOR_VERSION << "." << PATCH_LEVEL << " (" << BUILD_TYPE << ") beginning. 1.0 to 1.1: TDD/OpenCL/ext-LNB/faster added by Jiao Xianjun(putaoshu@gmail.com)" << endl;

// //    frequency information will be displayed in main() function.
//    if (freq_start==freq_end) {
//      cout << "  Search frequency: " << freq_start/1e6 << " MHz" << endl;
//    } else {
//      cout << "  Search frequency range: " << freq_start/1e6 << "-" << freq_end/1e6 << " MHz" << endl;
//    }

//    if (sampling_carrier_twist) {
      cout << "  PPM: " << ppm << endl;
      stringstream temp;
      temp << setprecision(20) << correction;
      cout << "  correction: " << temp.str() << endl;
//    }
    if (save_cap)
      cout << "  Captured data will be saved in capbufXXXX.it files" << endl;
    if (use_recorded_data)
      cout << "  Captured data will be read from capbufXXXX.it files" << endl;
  }
}

// In high SNR environments, a cell may be detected on different carrier
// frequencies and with different frequency offsets. Keep only the cell
// with the highest received power.
void dedup(
  const vector < list<Cell> > & detected_cells,
  list <Cell> & cells_final
) {
  cells_final.clear();
  for (uint16 t=0;t<detected_cells.size();t++) {
    list <Cell>::const_iterator it_n=detected_cells[t].begin();
    while (it_n!=detected_cells[t].end()) {
      bool match=false;
      list <Cell>::iterator it_f=cells_final.begin();
      while (it_f!=cells_final.end()) {
        // Do these detected cells match and are they close to each other
        // in frequency?
        if (
          ((*it_n).n_id_cell()==(*it_f).n_id_cell()) &&
          (abs(((*it_n).fc_requested+(*it_n).freq_superfine)-((*it_f).fc_requested+(*it_f).freq_superfine))<1e6)
        ) {
          match=true;
          // Keep either the new cell or the old cell, but not both.
          if ((*it_n).pss_pow>(*it_f).pss_pow) {
            (*it_f)=(*it_n);
          }
          break;
        }
        ++it_f;
      }
      if (!match) {
        // This cell does not match any previous cells. Add this to the
        // final list of cells.
        cells_final.push_back((*it_n));
      }
      ++it_n;
    }
  }
}

// Helper function to assist in formatting frequency offsets.
string freq_formatter(
  const double & freq
) {
  stringstream temp;
  if (abs(freq)<998.0) {
    temp << setw(5) << setprecision(3) << freq << "h";
  } else if (abs(freq)<998000.0) {
    temp << setw(5) << setprecision(3) << freq/1e3 << "k";
  } else if (abs(freq)<998000000.0) {
    temp << setw(5) << setprecision(3) << freq/1e6 << "m";
  } else if (abs(freq)<998000000000.0) {
    temp << setw(5) << setprecision(3) << freq/1e9 << "g";
  } else if (abs(freq)<998000000000000.0) {
    temp << setw(5) << setprecision(3) << freq/1e12 << "t";
  } else {
    temp << freq;
  }
  return temp.str();
}

// Open the USB device
void config_usb(
  const bool & sampling_carrier_twist,
  const double & correction,
  const int32 & device_index_cmdline,
  const double & fc,
  rtlsdr_dev_t *& dev,
  double & fs_programmed
) {
  int32 device_index=device_index_cmdline;

  int8 n_rtlsdr=rtlsdr_get_device_count();
  if (n_rtlsdr==0) {
    cerr << "Error: no RTL-SDR USB devices found..." << endl;
    ABORT(-1);
  }

  // Choose which device to use
  if ((n_rtlsdr==1)&&(device_index==-1)) {
    device_index=0;
  }
  if ((device_index<0)||(device_index>=n_rtlsdr)) {
    cerr << "Error: must specify which USB device to use with --device-index" << endl;
    cerr << "Found the following USB devices:" << endl;
    char vendor[256],product[256],serial[256];
    for (uint8 t=0;t<n_rtlsdr;t++) {
      rtlsdr_get_device_usb_strings(t,vendor,product,serial);
      cerr << "Device index " << t << ": [Vendor: " << vendor << "] [Product: " << product << "] [Serial#: " << serial << "]" << endl;
    }
    ABORT(-1);
  }

  // Open device
  if (rtlsdr_open(&dev,device_index)<0) {
    cerr << "Error: unable to open RTLSDR device" << endl;
    ABORT(-1);
  }

  double sampling_rate = 0;
//  if (sampling_carrier_twist)
    sampling_rate = (FS_LTE/16)*correction;
//  else
//    sampling_rate = 1920000;
  // Sampling frequency
  if (rtlsdr_set_sample_rate(dev,itpp::round(sampling_rate))<0) {
    cerr << "Error: unable to set sampling rate" << endl;
    ABORT(-1);
  }

  // Calculate the actual fs that was programmed
  fs_programmed=(double)rtlsdr_get_sample_rate(dev);

  // Center frequency
  uint8 n_fail=0;
  while (rtlsdr_set_center_freq(dev,itpp::round(fc*correction))<0) {
    n_fail++;
    if (n_fail>=5) {
      cerr << "Error: unable to set center frequency" << endl;
      ABORT(-1);
    }
    cerr << "Unable to set center frequency... retrying..." << endl;
    sleep(1);
  }

  // Turn on AGC
  if (rtlsdr_set_tuner_gain_mode(dev,0)<0) {
    cerr << "Error: unable to enter AGC mode" << endl;
    ABORT(-1);
  }

  // Reset the buffer
  if (rtlsdr_reset_buffer(dev)<0) {
    cerr << "Error: unable to reset RTLSDR buffer" << endl;
    ABORT(-1);
  }

  // Discard about 1.5s worth of data to give the AGC time to converge
  if (verbosity>=2) {
    cout << "Waiting for AGC to converge..." << endl;
  }
  uint32 n_read=0;
  int n_read_current;
#define BLOCK_SIZE 16*16384
  uint8 * buffer=(uint8 *)malloc(BLOCK_SIZE*sizeof(uint8));
  while (true) {
    if (rtlsdr_read_sync(dev,buffer,BLOCK_SIZE,&n_read_current)<0) {
      cerr << "Error: synchronous read failed" << endl;
      ABORT(-1);
    }
    if (n_read_current<BLOCK_SIZE) {
      cerr << "Error: short read; samples lost" << endl;
      ABORT(-1);
    }
    n_read+=n_read_current;
    if (n_read>2880000*2)
      break;
  }
  free(buffer);
}

// Main cell search routine.
int main(
  const int argc,
  char * const argv[]
) {
  // Command line parameters are stored here.
  double freq_start;
  double freq_end;
  uint16 num_try;
  bool sampling_carrier_twist;
  double ppm;
  double correction;
  bool save_cap;
  bool use_recorded_data;
  string data_dir;
  int device_index;
  char record_bin_filename[256] = {0};
  char load_bin_filename[256] = {0};
  uint16 opencl_platform;
  uint16 opencl_device;
  uint16 filter_workitem;
  uint16 xcorr_workitem;
  uint16 num_reserve;
  uint16 num_loop; // it is not so useful

  // Get search parameters from user
  parse_commandline(argc,argv,freq_start,freq_end,num_try,sampling_carrier_twist,ppm,correction,save_cap,use_recorded_data,data_dir,device_index, record_bin_filename, load_bin_filename,opencl_platform,opencl_device,filter_workitem,xcorr_workitem,num_reserve,num_loop);

  // Open the USB device (if necessary).
  rtlsdr_dev_t * dev=NULL;
  double fs_programmed = FS_LTE/16; // in case not initialized by config_usb
  double fc_programmed; // for correction frequency calculation
  double fc_requested, fc_requested_tmp, fc_programmed_tmp, fs_requested_tmp, fs_programmed_tmp;

  bool dongle_used = (!use_recorded_data) && (strlen(load_bin_filename)==0);
  if ( dongle_used && freq_start!=9999e6) {
    config_usb(sampling_carrier_twist,correction,device_index,freq_start,dev,fs_programmed);
    fc_programmed_tmp = calculate_fc_programmed_in_context(freq_start, use_recorded_data, load_bin_filename, dev);
    cout << "Use dongle begin with " << ( freq_start/1e6 ) << "MHz actual " << (fc_programmed_tmp/1e6) << "MHz " << fs_programmed << "MHz\n";
  } else {
    if (strlen(load_bin_filename)!=0) { // use captured bin file
      if ( read_header_from_bin( load_bin_filename, fc_requested_tmp, fc_programmed_tmp, fs_requested_tmp, fs_programmed_tmp) ) {
        cerr << "main: read_header_from_bin failed.\n";
        ABORT(-1);
      }
      if (fc_requested_tmp==NAN) { //  no valid header information is found
        cerr << "Neither frequency nor valid bin file header information is specified!\n";
        ABORT(-1);
      }
    } else if (use_recorded_data){ // use captured .it file

      stringstream filename;
      filename << data_dir << "/capbuf_" << setw(4) << setfill('0') << 0 << ".it";
      it_ifile itf(filename.str());

      itf.seek("fc");
      ivec fc_v;
      itf>>fc_v;

      itf.seek("fcp");
      ivec fc_p;
      itf>>fc_p;

      itf.seek("fsp");
      ivec fs_p;
      itf>>fs_p;

      itf.close();

      fc_requested_tmp = fc_v(0);
      fc_programmed_tmp = fc_p(0);
      fs_programmed_tmp = fs_p(0);

    }
    fs_programmed = fs_programmed_tmp;
    cout << "Use file begin with " << ( fc_requested_tmp/1e6 ) << "MHz actual " << (fc_programmed_tmp/1e6) << "MHz " << fs_programmed_tmp << "MHz\n";
  }

  if (use_recorded_data)
    num_try=1; // compatible to .it file case

  // Generate a list of center frequencies that should be searched and also
  // a list of frequency offsets that should be searched for each center
  // frequency.
  vec fc_search_set;
  double freq_correction = 0;
  if (freq_start!=9999e6) { // if frequency scanning range is specified
    fc_search_set=itpp_ext::matlab_range(freq_start,100e3,freq_end);
  } else { // if frequency scanning range is not specified. a file is as input
    if (strlen(load_bin_filename)!=0 || use_recorded_data) { // use captured file

        freq_start = fc_requested_tmp;
        freq_end = freq_start;
        fc_search_set.set_length(1, false);
        fc_search_set[0] = fc_requested_tmp;

    } else {
      cerr << "Neither frequency nor captured file is specified!\n";
      ABORT(-1);
    }
  }
  freq_correction = fc_programmed_tmp*(correction-1)/correction;

  cout << "    Search frequency: " << fc_search_set(0)/1e6 << " to " <<  fc_search_set( length(fc_search_set)-1 )/1e6 << " MHz" << endl;
  cout << "with freq correction: " << freq_correction/1e3 << " kHz" << endl;

  cmat pss_fo_set;// pre-generate frequencies offseted pss time domain sequence
  vec f_search_set;
  if (sampling_carrier_twist) { // original mode
    const uint16 n_extra=floor_i((fc_programmed_tmp*ppm/1e6+2.5e3)/5e3);
    f_search_set=to_vec(itpp_ext::matlab_range( -n_extra*5000,5000, (n_extra-1)*5000));
    // for graphic card which has limited mem, you should turn num_loop on if OpenCL reports -4: CL_MEM_OBJECT_ALLOCATION_FAILURE
//    if (num_loop == 0) // it is not so useful
//      num_loop = length(f_search_set)/2;
  } else {
    if (length(fc_search_set)==1) {//when only one frequency is specified, whole PPM range should be covered
      const uint16 n_extra=floor_i((fc_programmed_tmp*ppm/1e6+2.5e3)/5e3);
      f_search_set=to_vec(itpp_ext::matlab_range( -n_extra*5000,5000, (n_extra-1)*5000));
    } else {
      // since we have frequency step is 100e3, why not have sub search set limited by this regardless PPM?
      f_search_set=to_vec(itpp_ext::matlab_range(-60000,5000,55000)); // 2*65kHz > 100kHz, overlap adjacent frequencies
  //    if (num_loop == 0) // it is not so useful
  //      num_loop = 36;
  //      f_search_set=to_vec(itpp_ext::matlab_range(-100000,5000,100000)); // align to matlab script
    }
  }

  cout << "    Search PSS at fo: " << f_search_set(0)/1e3 << " to " << f_search_set( length(f_search_set)-1 )/1e3 << " kHz" << endl;

  pss_fo_set_gen(f_search_set, pss_fo_set);

//  cout << "Search PSS fo(crect): " << f_search_set(0)/1e3 << " to " << f_search_set( length(f_search_set)-1 )/1e3 << " kHz" << endl;

  const uint16 n_fc=length(fc_search_set);

  // construct data for multiple tries
  const uint32 n_fc_multi_try=n_fc*num_try;
  vec fc_search_set_multi_try;
  fc_search_set_multi_try.set_length(n_fc_multi_try, false);
  for(uint32 i=0; i<n_fc; i++) {
    uint32 sp = i*num_try;
    uint32 ep = sp + num_try;
    fc_search_set_multi_try.set_subvector(sp, ep-1, fc_search_set(i));
  }

  // get coefficients of 6 RB filter (to improve SNR)
  // coef = fir1(46, (0.18e6*6+150e3)/sampling_rate);
  vec coef(( sizeof( chn_6RB_filter_coef )/sizeof(float) ));
  for (uint16 i=0; i<length(coef); i++) {
    coef(i) = chn_6RB_filter_coef[i];
  }

  cvec capbuf;

  lte_opencl_t lte_ocl(opencl_platform, opencl_device);
  #ifdef USE_OPENCL
  lte_ocl.setup_filter_my((string)"filter_my_kernels.cl", CAPLENGTH, filter_workitem);

//  if ( (length(f_search_set)*3)%num_loop != 0 ){
//    cerr << "length(f_search_set)*3 can not be divided by num_loop. " << (length(f_search_set)*3) << " " << num_loop << "\n";
//    ABORT(-1);
//  }
//  lte_ocl.setup_filter_mchn((string)"filter_mchn_kernels.cl", CAPLENGTH, length(f_search_set)*3/num_loop, pss_fo_set.cols(), xcorr_workitem);
  lte_ocl.setup_filter_mchn((string)"filter_mchn_kernels.cl", CAPLENGTH, length(f_search_set)*3, pss_fo_set.cols(), xcorr_workitem);
  #endif

  vec period_ppm;
  vec k_factor_set;

  // Calculate the threshold vector
  const uint8 thresh1_n_nines=12;
  double rx_cutoff=(6*12*15e3/2+4*15e3)/(FS_LTE/16/2);

  // for PSS correlate
  //cout << "DS_COMB_ARM override!!!" << endl;
#define DS_COMB_ARM 2
  mat xc_incoherent_collapsed_pow;
  imat xc_incoherent_collapsed_frq;
  vector <mat>  xc_incoherent_single(3);
  vector <mat>  xc_incoherent(3);
  vector <mat> xc(3);
  vec sp_incoherent;
  vec sp;

  // for SSS detection
#define THRESH2_N_SIGMA 3
  vec sss_h1_np_est_meas;
  vec sss_h2_np_est_meas;
  cvec sss_h1_nrm_est_meas;
  cvec sss_h2_nrm_est_meas;
  cvec sss_h1_ext_est_meas;
  cvec sss_h2_ext_est_meas;
  mat log_lik_nrm;
  mat log_lik_ext;

  // for time frequency grid
  // Extract time and frequency grid
  cmat tfg;
  vec tfg_timestamp;
  // Compensate for time and frequency offsets
  cmat tfg_comp;
  vec tfg_comp_timestamp;

  Real_Timer tt; // for profiling
  // Each center frequency is searched independently. Results are stored in this vector.
  vector < list<Cell> > detected_cells(n_fc);
  // Loop for each center frequency.
  for (uint32 fci=0;fci<n_fc_multi_try;fci++) {
    fc_requested=fc_search_set_multi_try(fci);
    uint32 fc_idx = fci/num_try;
    uint32 try_idx = fci - fc_idx*num_try;

    if (verbosity>=1) {
      cout << "Examining center frequency " << fc_requested/1e6 << " MHz ... try " << try_idx << endl;
    }

    // Fill capture buffer
    int run_out_of_data = capture_data(fc_requested,correction,save_cap,record_bin_filename,use_recorded_data,load_bin_filename,data_dir,dev,capbuf,fc_programmed, fs_programmed, false);
    if (run_out_of_data){
      fci = n_fc_multi_try; // end of loop
      continue;
    }

    freq_correction = fc_programmed*(correction-1)/correction;
//    if (!dongle_used) { // if dongle is not used, do correction explicitly. Because if dongle is used, the correction is done when tuning dongle's frequency.
      capbuf = fshift(capbuf,-freq_correction,fs_programmed);
//    }

    // 6RB filter to improve SNR
    tt.tic();
    #ifdef USE_OPENCL
      lte_ocl.filter_my(capbuf); // be careful! capbuf.zeros() will slow down the xcorr part pretty much!
    #else
      filter_my(coef, capbuf);
    #endif
    cout << "6RB filter cost " << tt.get_time() << "s\n";

    vec dynamic_f_search_set = f_search_set; // don't touch the original
    double xcorr_pss_time;
    sampling_ppm_f_search_set_by_pss(lte_ocl, num_loop, capbuf, pss_fo_set, sampling_carrier_twist, num_reserve, dynamic_f_search_set, period_ppm, xc, xcorr_pss_time);
    cout << "PSS XCORR  cost " << xcorr_pss_time << "s\n";

    list <Cell> peak_search_cells;
    if (!sampling_carrier_twist) {
      if ( isnan(period_ppm[0]) ) {
        if (verbosity>=2) cout << "No valid PSS is found at pre-proc phase! Please try again.\n";
        continue;
      } else {
        k_factor_set.set_length(length(period_ppm));
        k_factor_set = 1 + period_ppm*1e-6;
      }

      vec tmp_f_search(1);
      vector <mat> tmp_xc(3);
      tmp_xc[0].set_size(1, length(capbuf)-136);
      tmp_xc[1].set_size(1, length(capbuf)-136);
      tmp_xc[2].set_size(1, length(capbuf)-136);
      for (uint16 i=0; i<length(k_factor_set); i++) {

        tmp_f_search(0) = dynamic_f_search_set(i);
        tmp_xc[0].set_row(0, xc[0].get_row(i));
        tmp_xc[1].set_row(0, xc[1].get_row(i));
        tmp_xc[2].set_row(0, xc[2].get_row(i));

        // Correlate
        uint16 n_comb_xc;
        uint16 n_comb_sp;
        if (verbosity>=2) {
          cout << "  Calculating PSS correlations" << endl;
        }
        xcorr_pss(capbuf,tmp_f_search,DS_COMB_ARM,fc_requested,fc_programmed,fs_programmed,tmp_xc,xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,xc_incoherent_single,xc_incoherent,sp_incoherent,sp,n_comb_xc,n_comb_sp,sampling_carrier_twist,(const double)k_factor_set[i]);

        // Calculate the threshold vector
        double R_th1=chi2cdf_inv(1-pow(10.0,-thresh1_n_nines),2*n_comb_xc*(2*DS_COMB_ARM+1));
        vec Z_th1=R_th1*sp_incoherent/rx_cutoff/137/2/n_comb_xc/(2*DS_COMB_ARM+1);

        // Search for the peaks
        if (verbosity>=2) {
          cout << "  Searching for and examining correlation peaks..." << endl;
        }
        peak_search(xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,Z_th1,tmp_f_search,fc_requested,fc_programmed,xc_incoherent_single,DS_COMB_ARM,sampling_carrier_twist,(const double)k_factor_set[i],peak_search_cells);

      }

    } else {

      // Correlate
      uint16 n_comb_xc;
      uint16 n_comb_sp;
      if (verbosity>=2) {
        cout << "  Calculating PSS correlations" << endl;
      }
      xcorr_pss(capbuf,dynamic_f_search_set,DS_COMB_ARM,fc_requested,fc_programmed,fs_programmed,xc,xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,xc_incoherent_single,xc_incoherent,sp_incoherent,sp,n_comb_xc,n_comb_sp,sampling_carrier_twist,NAN);

      // Calculate the threshold vector
      double R_th1=chi2cdf_inv(1-pow(10.0,-thresh1_n_nines),2*n_comb_xc*(2*DS_COMB_ARM+1));
      vec Z_th1=R_th1*sp_incoherent/rx_cutoff/137/2/n_comb_xc/(2*DS_COMB_ARM+1);

      // Search for the peaks
      if (verbosity>=2) {
        cout << "  Searching for and examining correlation peaks..." << endl;
      }
      peak_search(xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,Z_th1,dynamic_f_search_set,fc_requested,fc_programmed,xc_incoherent_single,DS_COMB_ARM,sampling_carrier_twist,NAN,peak_search_cells);

    }

    detected_cells[fc_idx]=peak_search_cells;

    // Loop and check each peak
    list<Cell>::iterator iterator=detected_cells[fc_idx].begin();
    Cell cell_temp(*iterator);
    while (iterator!=detected_cells[fc_idx].end()) {
      //cout << "Further examining: " << endl;
      //cout << (*iterator) << endl << endl;

      // Detect SSS if possible
      #define THRESH2_N_SIGMA 3
      int tdd_flag = 0;
      cell_temp = (*iterator);
      for(tdd_flag=0;tdd_flag<2;tdd_flag++)
      {
        (*iterator)=sss_detect(cell_temp,capbuf,THRESH2_N_SIGMA,fc_requested,fc_programmed,fs_programmed,sss_h1_np_est_meas,sss_h2_np_est_meas,sss_h1_nrm_est_meas,sss_h2_nrm_est_meas,sss_h1_ext_est_meas,sss_h2_ext_est_meas,log_lik_nrm,log_lik_ext,sampling_carrier_twist,tdd_flag);
        if ((*iterator).n_id_1!=-1)
            break;
      }
      if ((*iterator).n_id_1==-1) {
        // No SSS detected.
        iterator=detected_cells[fc_idx].erase(iterator);
        continue;
      }
      // Fine FOE
      (*iterator)=pss_sss_foe((*iterator),capbuf,fc_requested,fc_programmed,fs_programmed,sampling_carrier_twist,tdd_flag);

      // Extract time and frequency grid
      extract_tfg((*iterator),capbuf,fc_requested,fc_programmed,fs_programmed,tfg,tfg_timestamp,sampling_carrier_twist);

      // Create object containing all RS
      RS_DL rs_dl((*iterator).n_id_cell(),6,(*iterator).cp_type);

      // Compensate for time and frequency offsets
      (*iterator)=tfoec((*iterator),tfg,tfg_timestamp,fc_requested,fc_programmed,rs_dl,tfg_comp,tfg_comp_timestamp,sampling_carrier_twist);

      // Finally, attempt to decode the MIB
      (*iterator)=decode_mib((*iterator),tfg_comp,rs_dl);
      if ((*iterator).n_rb_dl==-1) {
        // No MIB could be successfully decoded.
        iterator=detected_cells[fc_idx].erase(iterator);
        continue;
      }

      if (verbosity>=1) {
        if (tdd_flag==0)
            cout << "  Detected a FDD cell! At freqeuncy " << fc_requested/1e6 << "MHz, try " << try_idx << endl;
        else
            cout << "  Detected a TDD cell! At freqeuncy " << fc_requested/1e6 << "MHz, try " << try_idx << endl;
        cout << "    cell ID: " << (*iterator).n_id_cell() << endl;
        cout << "     PSS ID: " << (*iterator).n_id_2 << endl;
        cout << "    RX power level: " << db10((*iterator).pss_pow) << " dB" << endl;
        cout << "    residual frequency offset: " << (*iterator).freq_superfine << " Hz" << endl;
        cout << "                     k_factor: " << (*iterator).k_factor << endl;
      }

      ++iterator;
    }
    if (detected_cells[fc_idx].size() > 0){
      fci = (fc_idx+1)*num_try - 1; // skip to next frequency
    }
  }

  // Generate final list of detected cells.
  list <Cell> cells_final;
  dedup(detected_cells,cells_final);

  // Print out the final list of detected cells.
  if (cells_final.size()==0) {
    cout << "No LTE cells were found..." << endl;
  } else {
    cout << "Detected the following cells:" << endl;
    cout << "DPX:TDD/FDD; A: #antenna ports C: CP type ; P: PHICH duration ; PR: PHICH resource type" << endl;
    cout << "DPX CID A      fc   freq-offset RXPWR C nRB P  PR CrystalCorrectionFactor" << endl;
    list <Cell>::iterator it=cells_final.begin();
    while (it!=cells_final.end()) {
      // Use a stringstream to avoid polluting the iostream settings of cout.
      stringstream ss;
      if ((*it).duplex_mode == 1)
        ss << "TDD ";
      else
        ss << "FDD ";
      ss << setw(3) << (*it).n_id_cell();
      ss << setw(2) << (*it).n_ports;
      ss << " " << setw(6) << setprecision(5) << (*it).fc_requested/1e6 << "M";
      ss << " " << setw(13) << freq_formatter((*it).freq_superfine);
      ss << " " << setw(5) << setprecision(3) << db10((*it).pss_pow);
      ss << " " << (((*it).cp_type==cp_type_t::NORMAL)?"N":(((*it).cp_type==cp_type_t::UNKNOWN)?"U":"E"));
      ss << " " << setw(3) << (*it).n_rb_dl;
      ss << " " << (((*it).phich_duration==phich_duration_t::NORMAL)?"N":(((*it).phich_duration==phich_duration_t::UNKNOWN)?"U":"E"));
      switch ((*it).phich_resource) {
        case phich_resource_t::UNKNOWN: ss << " UNK"; break;
        case phich_resource_t::oneSixth: ss << " 1/6"; break;
        case phich_resource_t::half: ss << " 1/2"; break;
        case phich_resource_t::one: ss << " one"; break;
        case phich_resource_t::two: ss << " two"; break;
      }

      // Calculate the correction factor.
      // This is where we know the carrier is located
      const double true_location=(*it).fc_programmed;
      // We can calculate the RTLSDR's actualy frequency
      const double crystal_freq_actual=(*it).fc_programmed-(*it).freq_superfine;
      // Calculate correction factors
      const double correction_residual=true_location/crystal_freq_actual;
      double correction_new=correction*correction_residual;
//      if (!sampling_carrier_twist) {
//        correction_new = (*it).k_factor;
//      }
      ss << " " << setprecision(20) << correction_new;
      cout << ss.str() << endl;

      ++it;
    }
  }

  // Successful exit.
  return 0;
}

