require "English"
require "stringio"
require "svn/error"
require "svn/util"
require "svn/ext/core"

module Svn
  module Core
    Util.set_constants(Ext::Core, self)
    Util.set_methods(Ext::Core, self)

    apr_initialize
    at_exit do
      if $DEBUG
        i = 0
        loop do
          i += 1
          print "number of pools before GC(#{i}): "
          before_pools = ObjectSpace.each_object(Svn::Core::Pool) {}
          p before_pools
          GC.start
          after_pools = ObjectSpace.each_object(Svn::Core::Pool) {}
          print "number of pools after GC(#{i}): "
          p after_pools
          break if before_pools == after_pools
        end
        puts "GC ran #{i} times"
      end
      
      # We don't need to call apr_termintae because pools
      # are destroyed by ruby's GC.
      # Svn::Core.apr_terminate
    end
    nls_init
    
    class << self
      alias binary_mime_type? mime_type_is_binary
    end


    DEFAULT_CHARSET = default_charset
    LOCALE_CHARSET = locale_charset
    
    AuthCredSSLClientCert = AuthCredSslClientCert
    AuthCredSSLClientCertPw = AuthCredSslClientCertPw
    AuthCredSSLServerTrust = AuthCredSslServerTrust
    
    
    Pool = Svn::Ext::Core::Apr_pool_t

    Stream = SWIG::TYPE_p_svn_stream_t

    class Stream
      CHUNK_SIZE = Core::STREAM_CHUNK_SIZE

      def write(data)
        Core.stream_close(self)
      end
      
      def read(len=nil)
        if len.nil?
          read_all
        else
          buf = ""
          while len > CHUNK_SIZE
            buf << _read(CHUNK_SIZE)
            len -= CHUNK_SIZE
          end
          buf << _read(len)
          buf
        end
      end
      
      def close
        Core.stream_close(self)
      end

      def copy(other)
        Core.stream_copy(self, other)
      end
      
      private
      def _read(size)
        Core.stream_read(self, size)
      end
      
      def read_all
        buf = ""
        while chunk = _read(CHUNK_SIZE)
          buf << chunk
        end
        buf
      end
    end


    AuthBaton = SWIG::TYPE_p_svn_auth_baton_t
    class AuthBaton
      class << self
        def new(providers=[], *rest)
          baton = Core.auth_open(providers)
          baton.__send__("initialize", providers, *rest)
          baton
        end
      end

      attr_reader :parameters
      def initialize(providers, parameters={})
        @providers = providers
        self.parameters = parameters
      end

      def [](name)
        Core.auth_get_parameter(self, name)
      end

      def []=(name, value)
        Core.auth_set_parameter(self, name, value)
        @parameters[name] = value
      end

      def parameters=(params)
        @parameters = {}
        params.each do |key, value|
          self[key] = value
        end
      end
    end
    

    class AuthProviderObject
      class << self
        undef new
      end
    end


    Diff = SWIG::TYPE_p_svn_diff_t
    class Diff
      attr_accessor :original, :modified

      class << self
        def file_diff(original, modified)
          diff = Core.diff_file_diff(original, modified)
          if diff
            diff.original = original
            diff.modified = modified
          end
          diff
        end
      end
      
      def unified(orig_label, mod_label)
        output = StringIO.new
        args = [
          output, self, @original, @modified,
          orig_label, mod_label,
        ]
        Core.diff_file_output_unified(*args)
        output.rewind
        output.read
      end

      def conflict?
        Core.diff_contains_conflicts(self)
      end

      def diff?
        Core.diff_contains_diffs(self)
      end
    end

    class Version

      alias _initialize initialize
      def initialize(major=nil, minor=nil, patch=nil, tag=nil)
        _initialize
        self.major = major if major
        self.minor = minor if minor
        self.patch = patch if patch
        self.tag = tag || ""
      end

      def ==(other)
        valid? and other.valid? and Core.ver_equal(self, other)
      end

      def compatible?(other)
        valid? and other.valid? and Core.ver_compatible(self, other)
      end

      def valid?
        (major and minor and patch and tag) ? true : false
      end

      alias _tag= tag=
      def tag=(value)
        @tag = value
        self._tag = value
      end

      def to_a
        [major, minor, patch, tag]
      end
      
      def to_s
        "#{major}.#{minor}.#{patch}#{tag}"
      end
    end

    class Dirent
      def directory?
        kind == NODE_DIR
      end

      def file?
        kind == NODE_FILE
      end
    end
  end
end
