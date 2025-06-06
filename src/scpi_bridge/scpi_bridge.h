#include <SCPI_Parser.h>
#include <list>
#include <riden_scpi/riden_scpi.h>
#include <vxi11_server/vxi_server.h>

class SCPI_handler: public SCPI_handler_interface
{
  public:
    SCPI_handler(RidenDongle::RidenScpi* ridenScpi): _ridenScpi(ridenScpi) {}

    void write(const char *data, size_t len) override
    {
        _ridenScpi->write(data, len);
    }
    scpi_result_t read(char *data, size_t *len, size_t max_len) override
    {
        return _ridenScpi->read(data, len, max_len);
    }
    bool claim_control() override
    {
        return _ridenScpi->claim_external_control();
    }
    void release_control() override
    {
        _ridenScpi->release_external_control();
    }

  private:
  RidenDongle::RidenScpi* _ridenScpi;
};


