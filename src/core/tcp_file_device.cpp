#include "core/tcp_file_device.h"
#include "core/blob.h"
#include "core/ifile.h"
#include "core/ifile_system_defines.h"
#include "core/file_system.h"
#include "core/tcp_connector.h"
#include "core/tcp_stream.h"


namespace Lux
{
	namespace FS
	{
		class TCPFile : public IFile
		{
		public:
			TCPFile(Net::TCPStream* stream) : m_stream(stream) {}
			~TCPFile() {}

			virtual bool open(const char* path, Mode mode) LUX_OVERRIDE
			{
				int32_t op = TCPCommand::OpenFile;
				int32_t ret = 0;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(mode);
				m_blob.write(path);

				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
				m_stream->read(m_file);

				return -1 != m_file;
			}

			virtual void close() LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Close;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);

				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
			}

			virtual bool read(void* buffer, size_t size) LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Read;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);
				m_blob.write(size);

				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
				return m_stream->read(buffer, size);
			}

			virtual bool write(const void* buffer, size_t size) LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Write;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);
				m_blob.write(size);
				m_blob.write(buffer, size);

				return m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());

			}

			virtual const void* getBuffer() const LUX_OVERRIDE
			{
				return NULL;
			}

			virtual size_t size() LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Size;
				uint32_t size = 0;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);

				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
				m_stream->read(size);

				return size;
			}

			virtual size_t seek(SeekMode base, int32_t pos) LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Seek;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);
				m_blob.write(base);
				m_blob.write(pos);

				size_t ret = 0;
				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
				m_stream->read(ret);

				return ret;
			}

			virtual size_t pos() LUX_OVERRIDE
			{
				int32_t op = TCPCommand::Seek;
				size_t pos = 0;

				m_blob.flush();
				m_blob.write(op);
				m_blob.write(m_file);

				m_stream->write(m_blob.getBuffer(), m_blob.getBufferSize());
				m_stream->read(pos);

				return pos;
			}

		private:
			Net::TCPStream* m_stream;
			Blob m_blob;
			uint32_t m_file;
		};

		struct TCPImpl
		{
			Net::TCPConnector m_connector;
			Net::TCPStream* m_stream;
		};

		IFile* TCPFileDevice::createFile(IFile* child)
		{
			return LUX_NEW(TCPFile)(m_impl->m_stream);
		}

		void TCPFileDevice::connect(const char* ip, uint16_t port)
		{
			m_impl = LUX_NEW(TCPImpl);
			m_impl->m_stream = m_impl->m_connector.connect(ip, port);
		}

		void TCPFileDevice::disconnect()
		{
			m_impl->m_stream->write(TCPCommand::Disconnect);
			LUX_DELETE(m_impl);
		}
	} // namespace FS
} // ~namespace Lux