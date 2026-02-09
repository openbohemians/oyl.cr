module Yam
  class Error < Exception
  end

  class ParseException < Error
    getter line_number : Int32
    getter column_number : Int32

    def initialize(message : String, @line_number : Int32, @column_number : Int32)
      super("#{message} at line #{@line_number}, column #{@column_number}")
    end

    def location : {Int32, Int32}
      {line_number, column_number}
    end
  end
end
