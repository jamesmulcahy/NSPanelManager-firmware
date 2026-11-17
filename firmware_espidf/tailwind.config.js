/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ["./web/**/*.{html,js}"],
  theme: {
    extend: {},
  },
  darkMode: 'selector',
  plugins: [require('@tailwindcss/forms')],
}

